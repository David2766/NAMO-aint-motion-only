#include "site_directory.h"
#include "site_group_summary.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef USE_ESP32
#include "radar_storage.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"

#include <new>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

bool configuration_contains_group(
    const SiteConfigurationSnapshot &configuration, const char *group_id) {
  if (group_id == nullptr || group_id[0] == '\0')
    return false;
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE &&
        member.spatial_profile.committed &&
        std::strcmp(member.spatial_profile.group_id, group_id) == 0)
      return true;
  }
  return false;
}

bool directory_floorplan_is_registered(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory, const char *floorplan_id) {
  if (floorplan_id == nullptr || floorplan_id[0] == '\0')
    return false;
  if (configuration.floorplan.committed &&
      std::strcmp(configuration.floorplan.floorplan_id, floorplan_id) == 0)
    return true;
  for (const auto &group : directory.groups) {
    if (group.occupied &&
        std::strcmp(group.floorplan_id, floorplan_id) == 0)
      return true;
  }
  return false;
}

constexpr uint8_t DIRECTORY_MAGIC[] = {'N', 'A', 'M', 'D'};
constexpr uint8_t COMMIT_MAGIC[] = {'N', 'M', 'D', 'C'};
constexpr uint16_t COMMIT_VERSION = 1;
constexpr std::size_t HEADER_SIZE = 24;
constexpr std::size_t FLOORPLAN_HEADER_SIZE = 20;
constexpr std::size_t GROUP_HEADER_SIZE = 12;
constexpr std::size_t CHECKSUM_SIZE = 4;
constexpr uint8_t DIRECTORY_FLAG_COMMITTED = 0x01;
constexpr uint8_t DIRECTORY_FLAG_COMPATIBILITY_MIRROR = 0x02;
constexpr uint8_t DIRECTORY_FLAG_LEGACY_SINGLE_FLOORPLAN_PROJECTION = 0x04;
constexpr uint8_t FLOORPLAN_FLAG_COMMITTED = 0x01;

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
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t value = 2166136261UL;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= data[index];
    value *= 16777619UL;
  }
  return value;
}

void hash_byte(uint32_t *value, uint8_t byte) {
  *value ^= byte;
  *value *= 16777619UL;
}

void hash_u32(uint32_t *value, uint32_t number) {
  for (uint8_t shift = 0; shift < 4; ++shift)
    hash_byte(value, static_cast<uint8_t>(number >> (shift * 8U)));
}

void hash_text(uint32_t *value, const char *text) {
  if (text != nullptr) {
    while (*text != '\0')
      hash_byte(value, static_cast<uint8_t>(*text++));
  }
  hash_byte(value, 0);
}

bool all_bytes_are(const uint8_t *data, std::size_t size, uint8_t value) {
  for (std::size_t index = 0; index < size; ++index) {
    if (data[index] != value)
      return false;
  }
  return true;
}

bool valid_text(const char *value, std::size_t maximum) {
  if (value == nullptr)
    return false;
  const std::size_t length = std::strlen(value);
  if (length == 0 || length > maximum)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const char character = value[index];
    const bool alpha_numeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    if (!alpha_numeric &&
        (index == 0 || (character != '.' && character != '_' &&
                        character != ':' && character != '-')))
      return false;
  }
  return true;
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  target[0] = '\0';
  if (source == nullptr)
    return;
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

bool same_text(const char *left, const char *right) {
  return std::strcmp(left, right) == 0;
}

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool valid_floorplan(const FloorplanDirectoryEntry &entry) {
  if (!entry.occupied)
    return false;
  if (!valid_text(entry.floorplan_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text(entry.owner_node_id, ParticipantRecord::MAX_ID_LENGTH) ||
      entry.owner_generation == 0)
    return false;
  if (entry.committed)
    return entry.geometry_version > 0 && entry.config_hash != 0 &&
           entry.image_hash != 0;
  return entry.geometry_version == 0 && entry.config_hash == 0 &&
         entry.image_hash == 0;
}

bool valid_group(const GroupDirectoryEntry &entry) {
  return entry.occupied &&
         valid_text(entry.group_id,
                    ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) &&
         valid_text(entry.floorplan_id, ParticipantRecord::MAX_ID_LENGTH) &&
         valid_text(entry.leader_node_id, ParticipantRecord::MAX_ID_LENGTH) &&
         entry.assignment_revision > 0 && entry.member_count > 0 &&
         entry.member_count <= SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS;
}

bool compatibility_mirror_matches_configuration(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory) {
  if (!directory.compatibility_mirror ||
      directory.legacy_single_floorplan_projection ||
      !same_text(directory.site_id, configuration.site_id) ||
      directory.topology_revision != configuration.roster_revision ||
      directory.committed != configuration.roster_committed ||
      directory.group_count() != 0)
    return false;
  if (!configuration.floorplan.committed)
    return directory.floorplan_count() == 0;
  if (directory.floorplan_count() != 1)
    return false;
  const auto &floorplan = directory.floorplans[0];
  return floorplan.occupied && floorplan.committed &&
         same_text(floorplan.floorplan_id,
                   configuration.floorplan.floorplan_id) &&
         same_text(floorplan.owner_node_id,
                   configuration.configuration_owner_node_id) &&
         floorplan.owner_generation == configuration.generation &&
         floorplan.geometry_version ==
             configuration.floorplan.geometry_version &&
         floorplan.config_hash == configuration.floorplan.config_hash &&
         floorplan.image_hash == configuration.floorplan.image_hash;
}

bool legacy_projection_metadata_matches_configuration(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory) {
  if (!directory.legacy_single_floorplan_projection ||
      !same_text(directory.site_id, configuration.site_id) ||
      directory.topology_revision != configuration.roster_revision ||
      !directory.committed || !configuration.roster_committed ||
      !configuration.floorplan.committed ||
      directory.floorplan_count() != 1)
    return false;
  const auto &floorplan = directory.floorplans[0];
  return floorplan.occupied && floorplan.committed &&
         same_text(floorplan.floorplan_id,
                   configuration.floorplan.floorplan_id) &&
         same_text(floorplan.owner_node_id,
                   configuration.configuration_owner_node_id) &&
         floorplan.owner_generation == configuration.generation &&
         floorplan.geometry_version ==
             configuration.floorplan.geometry_version &&
         floorplan.config_hash == configuration.floorplan.config_hash &&
         floorplan.image_hash == configuration.floorplan.image_hash;
}

bool same_floorplan(const FloorplanDirectoryEntry &left,
                    const FloorplanDirectoryEntry &right) {
  if (left.occupied != right.occupied)
    return false;
  if (!left.occupied)
    return true;
  return left.committed == right.committed &&
         same_text(left.floorplan_id, right.floorplan_id) &&
         same_text(left.owner_node_id, right.owner_node_id) &&
         left.owner_generation == right.owner_generation &&
         left.geometry_version == right.geometry_version &&
         left.config_hash == right.config_hash &&
         left.image_hash == right.image_hash;
}

bool same_group(const GroupDirectoryEntry &left,
                const GroupDirectoryEntry &right) {
  if (left.occupied != right.occupied)
    return false;
  if (!left.occupied)
    return true;
  return same_text(left.group_id, right.group_id) &&
         same_text(left.floorplan_id, right.floorplan_id) &&
         same_text(left.leader_node_id, right.leader_node_id) &&
         left.assignment_revision == right.assignment_revision &&
         left.member_count == right.member_count;
}

bool same_directory_payload(const SiteDirectorySnapshot &left,
                            const SiteDirectorySnapshot &right) {
  if (!same_text(left.site_id, right.site_id) ||
      left.topology_revision != right.topology_revision ||
      left.committed != right.committed ||
      left.compatibility_mirror != right.compatibility_mirror ||
      left.legacy_single_floorplan_projection !=
          right.legacy_single_floorplan_projection)
    return false;
  for (std::size_t index = 0; index < left.floorplans.size(); ++index) {
    if (!same_floorplan(left.floorplans[index], right.floorplans[index]))
      return false;
  }
  for (std::size_t index = 0; index < left.groups.size(); ++index) {
    if (!same_group(left.groups[index], right.groups[index]))
      return false;
  }
  return true;
}

const GroupCoordinatorState *find_group_state(
    const GroupCoordinator &coordinator, const char *group_id) {
  for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
    const auto *state = coordinator.group_at(slot);
    if (state != nullptr && same_text(state->group_id, group_id))
      return state;
  }
  return nullptr;
}

bool member_is_active_spatial_input(
    const SiteConfigurationMember &member) {
  return member.occupied &&
         member.membership == ParticipantMembership::ACTIVE &&
         member.spatial_profile.committed &&
         member.spatial_profile.group_id[0] != '\0';
}

void encode_commit(uint8_t slot, uint32_t generation,
                   SiteDirectoryCommitBlob *blob) {
  blob->bytes.fill(0xFF);
  std::memcpy(blob->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(blob->bytes.data() + 4, COMMIT_VERSION);
  blob->bytes[6] = slot;
  blob->bytes[7] = 0;
  write_u32(blob->bytes.data() + 8, generation);
  write_u32(blob->bytes.data() + 12, fnv1a(blob->bytes.data(), 12));
}

bool decode_commit(const SiteDirectoryCommitBlob &blob, uint8_t *slot,
                   uint32_t *generation) {
  if (slot == nullptr || generation == nullptr ||
      std::memcmp(blob.bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0 ||
      read_u16(blob.bytes.data() + 4) != COMMIT_VERSION ||
      blob.bytes[6] > 1 || blob.bytes[7] != 0 ||
      read_u32(blob.bytes.data() + 12) != fnv1a(blob.bytes.data(), 12))
    return false;
  *slot = blob.bytes[6];
  *generation = read_u32(blob.bytes.data() + 8);
  return *generation != 0;
}

}  // namespace

SiteFloorplanOwnerResult SiteFloorplanOwnerCache::accept(
    const SiteConfigurationSnapshot &configuration,
    const SiteFloorplanOwnerReport &report, uint32_t now_ms) {
  if (!valid_text(report.participant_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text(report.floorplan_id, ParticipantRecord::MAX_ID_LENGTH) ||
      report.owner_generation == 0 || report.geometry_version == 0 ||
      report.config_hash == 0 || report.image_hash == 0 ||
      report.lease_ms < 2000 || report.lease_ms > 15000) {
    ++this->rejected_;
    return SiteFloorplanOwnerResult::INVALID_ARGUMENT;
  }
  const auto *participant = configuration.find(report.participant_id);
  if (participant == nullptr ||
      participant->membership != ParticipantMembership::ACTIVE ||
      participant->type != ParticipantType::NAMO_NODE ||
      !participant->leader_eligible) {
    ++this->rejected_;
    return SiteFloorplanOwnerResult::PARTICIPANT_INACTIVE;
  }
  for (const auto &owner : this->owners_) {
    if (!owner.occupied)
      continue;
    if (same_text(owner.floorplan.owner_node_id, report.participant_id) &&
        !same_text(owner.floorplan.floorplan_id, report.floorplan_id)) {
      ++this->rejected_;
      return SiteFloorplanOwnerResult::OWNER_CONFLICT;
    }
  }

  auto *state = this->ensure_(report.floorplan_id);
  if (state == nullptr) {
    ++this->rejected_;
    return SiteFloorplanOwnerResult::FULL;
  }
  if (state->occupied) {
    if (!same_text(state->floorplan.owner_node_id, report.participant_id)) {
      ++this->rejected_;
      return SiteFloorplanOwnerResult::FLOORPLAN_CONFLICT;
    }
    if (report.owner_generation < state->floorplan.owner_generation) {
      ++this->rejected_;
      return SiteFloorplanOwnerResult::STALE_GENERATION;
    }
    if (report.owner_generation == state->floorplan.owner_generation &&
        (state->floorplan.geometry_version != report.geometry_version ||
         state->floorplan.config_hash != report.config_hash ||
         state->floorplan.image_hash != report.image_hash)) {
      ++this->rejected_;
      return SiteFloorplanOwnerResult::REVISION_CONFLICT;
    }
  }

  const bool changed =
      !state->occupied ||
      state->floorplan.owner_generation != report.owner_generation ||
      state->floorplan.geometry_version != report.geometry_version ||
      state->floorplan.config_hash != report.config_hash ||
      state->floorplan.image_hash != report.image_hash;
  state->occupied = true;
  state->floorplan = {};
  state->floorplan.occupied = true;
  state->floorplan.committed = true;
  copy_text(state->floorplan.floorplan_id,
            sizeof(state->floorplan.floorplan_id), report.floorplan_id);
  copy_text(state->floorplan.owner_node_id,
            sizeof(state->floorplan.owner_node_id), report.participant_id);
  state->floorplan.owner_generation = report.owner_generation;
  state->floorplan.geometry_version = report.geometry_version;
  state->floorplan.config_hash = report.config_hash;
  state->floorplan.image_hash = report.image_hash;
  state->lease_expires_at_ms = now_ms + report.lease_ms;
  if (changed)
    ++this->generation_;
  return SiteFloorplanOwnerResult::OK;
}

void SiteFloorplanOwnerCache::reconcile(
    const SiteConfigurationSnapshot *configuration, bool local_authority,
    uint32_t now_ms) {
  if (!local_authority || configuration == nullptr ||
      !configuration->roster_committed) {
    this->clear();
    return;
  }
  bool changed = false;
  for (auto &owner : this->owners_) {
    if (!owner.occupied)
      continue;
    const auto *participant =
        configuration->find(owner.floorplan.owner_node_id);
    const bool expired = time_reached(now_ms, owner.lease_expires_at_ms);
    if (!expired && participant != nullptr &&
        participant->membership == ParticipantMembership::ACTIVE &&
        participant->type == ParticipantType::NAMO_NODE &&
        participant->leader_eligible)
      continue;
    if (expired)
      ++this->expired_;
    owner = {};
    changed = true;
  }
  if (changed)
    ++this->generation_;
}

void SiteFloorplanOwnerCache::clear() {
  bool changed = false;
  for (auto &owner : this->owners_) {
    if (!owner.occupied)
      continue;
    owner = {};
    changed = true;
  }
  if (changed)
    ++this->generation_;
}

const SiteFloorplanOwnerState *SiteFloorplanOwnerCache::find(
    const char *floorplan_id) const {
  if (floorplan_id == nullptr)
    return nullptr;
  for (const auto &owner : this->owners_) {
    if (owner.occupied &&
        same_text(owner.floorplan.floorplan_id, floorplan_id))
      return &owner;
  }
  return nullptr;
}

const SiteFloorplanOwnerState *SiteFloorplanOwnerCache::owner_at(
    std::size_t slot) const {
  return slot < this->owners_.size() && this->owners_[slot].occupied
             ? &this->owners_[slot]
             : nullptr;
}

std::size_t SiteFloorplanOwnerCache::size() const {
  std::size_t count = 0;
  for (const auto &owner : this->owners_)
    count += owner.occupied ? 1U : 0U;
  return count;
}

SiteFloorplanOwnerState *SiteFloorplanOwnerCache::find_mutable_(
    const char *floorplan_id) {
  return const_cast<SiteFloorplanOwnerState *>(this->find(floorplan_id));
}

SiteFloorplanOwnerState *SiteFloorplanOwnerCache::ensure_(
    const char *floorplan_id) {
  if (auto *existing = this->find_mutable_(floorplan_id))
    return existing;
  for (auto &owner : this->owners_) {
    if (!owner.occupied)
      return &owner;
  }
  return nullptr;
}

std::size_t SiteDirectorySnapshot::floorplan_count() const {
  std::size_t count = 0;
  for (const auto &entry : this->floorplans)
    count += entry.occupied ? 1U : 0U;
  return count;
}

std::size_t SiteDirectorySnapshot::group_count() const {
  std::size_t count = 0;
  for (const auto &entry : this->groups)
    count += entry.occupied ? 1U : 0U;
  return count;
}

const FloorplanDirectoryEntry *SiteDirectorySnapshot::find_floorplan(
    const char *floorplan_id) const {
  if (floorplan_id == nullptr)
    return nullptr;
  for (const auto &entry : this->floorplans) {
    if (entry.occupied && same_text(entry.floorplan_id, floorplan_id))
      return &entry;
  }
  return nullptr;
}

const GroupDirectoryEntry *SiteDirectorySnapshot::find_group(
    const char *group_id) const {
  if (group_id == nullptr)
    return nullptr;
  for (const auto &entry : this->groups) {
    if (entry.occupied && same_text(entry.group_id, group_id))
      return &entry;
  }
  return nullptr;
}

const char *site_directory_result_to_string(SiteDirectoryResult result) {
  switch (result) {
    case SiteDirectoryResult::OK:
      return "ok";
    case SiteDirectoryResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case SiteDirectoryResult::INVALID_DIRECTORY:
      return "invalid_directory";
    case SiteDirectoryResult::NOT_READY:
      return "not_ready";
    case SiteDirectoryResult::FULL:
      return "full";
    case SiteDirectoryResult::CONFLICT:
      return "conflict";
  }
  return "unknown";
}

bool valid_site_directory(const SiteDirectorySnapshot &directory) {
  if (!valid_text(directory.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      directory.generation == 0 ||
      (directory.compatibility_mirror &&
       directory.legacy_single_floorplan_projection) ||
      (directory.legacy_single_floorplan_projection &&
       (directory.floorplan_count() != 1 ||
        directory.group_count() == 0)))
    return false;

  for (std::size_t index = 0; index < directory.floorplans.size(); ++index) {
    const auto &entry = directory.floorplans[index];
    if (!entry.occupied)
      continue;
    if (!valid_floorplan(entry))
      return false;
    for (std::size_t other = index + 1;
         other < directory.floorplans.size(); ++other) {
      const auto &candidate = directory.floorplans[other];
      if (!candidate.occupied)
        continue;
      if (same_text(entry.floorplan_id, candidate.floorplan_id) ||
          same_text(entry.owner_node_id, candidate.owner_node_id))
        return false;
    }
  }

  for (std::size_t index = 0; index < directory.groups.size(); ++index) {
    const auto &entry = directory.groups[index];
    if (!entry.occupied)
      continue;
    if (!valid_group(entry) ||
        directory.find_floorplan(entry.floorplan_id) == nullptr)
      return false;
    for (std::size_t other = index + 1; other < directory.groups.size();
         ++other) {
      const auto &candidate = directory.groups[other];
      if (!candidate.occupied)
        continue;
      if (same_text(entry.group_id, candidate.group_id) ||
          same_text(entry.leader_node_id, candidate.leader_node_id))
        return false;
    }
  }
  return true;
}

bool site_directory_matches_configuration_scope(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory) {
  return configuration.site_id[0] != '\0' &&
         valid_site_directory(directory) &&
         std::strcmp(directory.site_id, configuration.site_id) == 0;
}

bool site_directory_is_canonical_for_configuration(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory) {
  return configuration.roster_committed && directory.committed &&
         directory.topology_revision != 0 &&
         !directory.compatibility_mirror &&
         site_directory_matches_configuration_scope(configuration, directory);
}

bool same_site_directory_payload(const SiteDirectorySnapshot &left,
                                 const SiteDirectorySnapshot &right) {
  return same_directory_payload(left, right);
}

uint32_t site_directory_payload_hash(
    const SiteDirectorySnapshot &directory) {
  uint32_t value = 2166136261UL;
  hash_text(&value, directory.site_id);
  hash_u32(&value, directory.topology_revision);
  hash_byte(&value, directory.committed ? 1 : 0);
  hash_byte(&value, directory.compatibility_mirror ? 1 : 0);
  hash_byte(&value,
            directory.legacy_single_floorplan_projection ? 1 : 0);
  for (const auto &floorplan : directory.floorplans) {
    hash_byte(&value, floorplan.occupied ? 1 : 0);
    if (!floorplan.occupied)
      continue;
    hash_byte(&value, floorplan.committed ? 1 : 0);
    hash_text(&value, floorplan.floorplan_id);
    hash_text(&value, floorplan.owner_node_id);
    hash_u32(&value, floorplan.owner_generation);
    hash_u32(&value, floorplan.geometry_version);
    hash_u32(&value, floorplan.config_hash);
    hash_u32(&value, floorplan.image_hash);
  }
  for (const auto &group : directory.groups) {
    hash_byte(&value, group.occupied ? 1 : 0);
    if (!group.occupied)
      continue;
    hash_text(&value, group.group_id);
    hash_text(&value, group.floorplan_id);
    hash_text(&value, group.leader_node_id);
    hash_u32(&value, group.assignment_revision);
    hash_byte(&value, group.member_count);
  }
  return value == 0 ? 1 : value;
}

uint32_t group_directory_assignment_hash(
    const GroupDirectoryEntry &group) {
  uint32_t value = 2166136261UL;
  hash_text(&value, group.group_id);
  hash_text(&value, group.floorplan_id);
  hash_text(&value, group.leader_node_id);
  hash_u32(&value, group.assignment_revision);
  hash_byte(&value, group.member_count);
  return value == 0 ? 1 : value;
}

bool group_directory_has_exact_shape(const GroupDirectoryEntry *group,
                                     uint8_t member_count) {
  return group != nullptr && group->assignment_revision != 0 &&
         member_count != 0 && group->member_count == member_count;
}

bool group_directory_is_expanding(const GroupDirectoryEntry *group,
                                  uint8_t member_count) {
  return group != nullptr && group->assignment_revision != 0 &&
         group->member_count != 0 && group->member_count < member_count;
}

SiteDirectoryResult migrate_site_configuration_to_directory(
    const SiteConfigurationSnapshot &configuration,
    SiteDirectorySnapshot *directory) {
  if (directory == nullptr ||
      !valid_text(configuration.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      configuration.generation == 0)
    return SiteDirectoryResult::INVALID_ARGUMENT;

  *directory = {};
  copy_text(directory->site_id, sizeof(directory->site_id),
            configuration.site_id);
  directory->generation = configuration.generation;
  directory->topology_revision = configuration.roster_revision;
  directory->committed = configuration.roster_committed;
  directory->compatibility_mirror = true;
  if (configuration.floorplan.committed) {
    if (!valid_text(configuration.configuration_owner_node_id,
                    ParticipantRecord::MAX_ID_LENGTH) ||
        !valid_text(configuration.floorplan.floorplan_id,
                    ParticipantRecord::MAX_ID_LENGTH))
      return SiteDirectoryResult::INVALID_DIRECTORY;
    auto &entry = directory->floorplans[0];
    entry.occupied = true;
    entry.committed = true;
    copy_text(entry.floorplan_id, sizeof(entry.floorplan_id),
              configuration.floorplan.floorplan_id);
    copy_text(entry.owner_node_id, sizeof(entry.owner_node_id),
              configuration.configuration_owner_node_id);
    entry.owner_generation = configuration.generation;
    entry.geometry_version = configuration.floorplan.geometry_version;
    entry.config_hash = configuration.floorplan.config_hash;
    entry.image_hash = configuration.floorplan.image_hash;
  }
  if (!valid_site_directory(*directory))
    return SiteDirectoryResult::INVALID_DIRECTORY;
  return SiteDirectoryResult::OK;
}

SiteDirectoryResult prepare_site_directory_compatibility_mirror(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *current, SiteDirectorySnapshot *candidate,
    bool *update_required) {
  if (candidate == nullptr || update_required == nullptr)
    return SiteDirectoryResult::INVALID_ARGUMENT;
  if (current != nullptr && !current->compatibility_mirror &&
      site_directory_matches_configuration_scope(configuration, *current)) {
    *candidate = *current;
    *update_required = false;
    return SiteDirectoryResult::OK;
  }
  const bool changed =
      current == nullptr ||
      !compatibility_mirror_matches_configuration(configuration, *current);
  const uint32_t current_generation =
      current != nullptr ? current->generation : 0;
  const auto result =
      migrate_site_configuration_to_directory(configuration, candidate);
  if (result != SiteDirectoryResult::OK)
    return result;
  if (current != nullptr)
    candidate->generation = current_generation;
  *update_required = changed;
  return SiteDirectoryResult::OK;
}

SiteDirectoryResult prepare_legacy_single_floorplan_projection(
    const SiteConfigurationSnapshot &configuration,
    const GroupCoordinator &coordinator,
    const SiteDirectorySnapshot &current, SiteDirectorySnapshot *candidate,
    bool *update_required) {
  if (candidate == nullptr || update_required == nullptr ||
      !valid_text(configuration.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      configuration.generation == 0 || current.generation == 0 ||
      !same_text(configuration.site_id, current.site_id))
    return SiteDirectoryResult::INVALID_ARGUMENT;

  if (!current.compatibility_mirror &&
      !current.legacy_single_floorplan_projection) {
    *candidate = current;
    *update_required = false;
    return SiteDirectoryResult::OK;
  }

  if (!configuration.roster_committed)
    return SiteDirectoryResult::NOT_READY;
  if (!configuration.floorplan.committed) {
    const auto result =
        migrate_site_configuration_to_directory(configuration, candidate);
    if (result != SiteDirectoryResult::OK)
      return result;
    candidate->generation = current.generation;
    *update_required = !same_directory_payload(current, *candidate);
    return SiteDirectoryResult::OK;
  }
  if (current.legacy_single_floorplan_projection &&
      !legacy_projection_metadata_matches_configuration(configuration,
                                                        current)) {
    const auto result =
        migrate_site_configuration_to_directory(configuration, candidate);
    if (result != SiteDirectoryResult::OK)
      return result;
    candidate->generation = current.generation;
    *update_required = !same_directory_payload(current, *candidate);
    return SiteDirectoryResult::OK;
  }
  if (!site_configuration_member_active(
          configuration, configuration.configuration_owner_node_id))
    return SiteDirectoryResult::NOT_READY;

  *candidate = {};
  copy_text(candidate->site_id, sizeof(candidate->site_id),
            configuration.site_id);
  candidate->generation = current.generation;
  candidate->topology_revision = configuration.roster_revision;
  candidate->committed = true;
  candidate->legacy_single_floorplan_projection = true;

  auto &floorplan = candidate->floorplans[0];
  floorplan.occupied = true;
  floorplan.committed = true;
  copy_text(floorplan.floorplan_id, sizeof(floorplan.floorplan_id),
            configuration.floorplan.floorplan_id);
  copy_text(floorplan.owner_node_id, sizeof(floorplan.owner_node_id),
            configuration.configuration_owner_node_id);
  floorplan.owner_generation = configuration.generation;
  floorplan.geometry_version = configuration.floorplan.geometry_version;
  floorplan.config_hash = configuration.floorplan.config_hash;
  floorplan.image_hash = configuration.floorplan.image_hash;

  std::size_t group_count = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE)
      continue;
    if (!member_is_active_spatial_input(member))
      return SiteDirectoryResult::NOT_READY;

    GroupDirectoryEntry *group = nullptr;
    for (auto &entry : candidate->groups) {
      if (entry.occupied &&
          same_text(entry.group_id, member.spatial_profile.group_id)) {
        group = &entry;
        break;
      }
    }
    if (group == nullptr) {
      if (group_count >= candidate->groups.size())
        return SiteDirectoryResult::FULL;
      group = &candidate->groups[group_count++];
      group->occupied = true;
      copy_text(group->group_id, sizeof(group->group_id),
                member.spatial_profile.group_id);
      copy_text(group->floorplan_id, sizeof(group->floorplan_id),
                configuration.floorplan.floorplan_id);
    }
    if (group->member_count >=
        SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS)
      return SiteDirectoryResult::FULL;
    ++group->member_count;
  }
  if (group_count == 0)
    return SiteDirectoryResult::NOT_READY;

  for (std::size_t index = 0; index < group_count; ++index) {
    auto &group = candidate->groups[index];
    const auto *state = find_group_state(coordinator, group.group_id);
    if (state == nullptr || !state->configured ||
        state->leader_node_id[0] == '\0' || state->term == 0 ||
        state->participants_expected != group.member_count)
      return SiteDirectoryResult::NOT_READY;
    const auto *leader = configuration.find(state->leader_node_id);
    if (leader == nullptr ||
        !site_configuration_member_active(configuration,
                                          state->leader_node_id, true) ||
        !leader->spatial_profile.committed ||
        !same_text(leader->spatial_profile.group_id, group.group_id))
      return SiteDirectoryResult::CONFLICT;
    copy_text(group.leader_node_id, sizeof(group.leader_node_id),
              state->leader_node_id);
    const auto *current_group = current.find_group(group.group_id);
    const bool same_assignment =
        current_group != nullptr &&
        same_text(current_group->floorplan_id, group.floorplan_id) &&
        same_text(current_group->leader_node_id, group.leader_node_id) &&
        current_group->member_count == group.member_count;
    if (same_assignment) {
      group.assignment_revision = current_group->assignment_revision;
    } else if (current_group == nullptr) {
      group.assignment_revision = 1;
    } else if (current_group->assignment_revision ==
               std::numeric_limits<uint32_t>::max()) {
      return SiteDirectoryResult::CONFLICT;
    } else {
      group.assignment_revision = current_group->assignment_revision + 1;
    }
  }
  std::sort(candidate->groups.begin(), candidate->groups.end(),
            [](const GroupDirectoryEntry &left,
               const GroupDirectoryEntry &right) {
              if (left.occupied != right.occupied)
                return left.occupied;
              if (!left.occupied)
                return false;
              return std::strcmp(left.group_id, right.group_id) < 0;
            });

  if (!valid_site_directory(*candidate))
    return SiteDirectoryResult::INVALID_DIRECTORY;
  *update_required = !same_directory_payload(current, *candidate);
  return SiteDirectoryResult::OK;
}

SiteDirectoryResult prepare_site_directory_from_advertisements(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &current,
    const SiteFloorplanOwnerCache &floorplan_owners,
    const SiteGroupSummaryCache &group_summaries,
    SiteDirectorySnapshot *candidate, bool *update_required) {
  if (candidate == nullptr || update_required == nullptr)
    return SiteDirectoryResult::INVALID_ARGUMENT;
  *update_required = false;
  if (!valid_site_directory(current) || !configuration.roster_committed ||
      std::strcmp(current.site_id, configuration.site_id) != 0)
    return SiteDirectoryResult::INVALID_DIRECTORY;
  if ((current.compatibility_mirror ||
       current.legacy_single_floorplan_projection) &&
      (floorplan_owners.size() == 0 || group_summaries.size() == 0))
    return SiteDirectoryResult::NOT_READY;

  *candidate = current;
  bool changed = current.compatibility_mirror ||
                 current.legacy_single_floorplan_projection;

  for (auto &group : candidate->groups) {
    if (group.occupied &&
        !configuration_contains_group(configuration, group.group_id)) {
      group = {};
      changed = true;
    }
  }
  for (auto &floorplan : candidate->floorplans) {
    if (floorplan.occupied && !directory_floorplan_is_registered(
                                  configuration, *candidate,
                                  floorplan.floorplan_id)) {
      floorplan = {};
      changed = true;
    }
  }

  for (std::size_t slot = 0;
       slot < SiteFloorplanOwnerCache::MAX_FLOORPLANS; ++slot) {
    const auto *advertisement = floorplan_owners.owner_at(slot);
    if (advertisement == nullptr)
      continue;
    auto *entry = const_cast<FloorplanDirectoryEntry *>(
        candidate->find_floorplan(
            advertisement->floorplan.floorplan_id));
    if (entry == nullptr) {
      for (auto &available : candidate->floorplans) {
        if (!available.occupied) {
          entry = &available;
          break;
        }
      }
      if (entry == nullptr)
        return SiteDirectoryResult::FULL;
      *entry = advertisement->floorplan;
      changed = true;
      continue;
    }
    if (std::strcmp(entry->owner_node_id,
                    advertisement->floorplan.owner_node_id) != 0) {
      if (std::strcmp(configuration.configuration_owner_node_id,
                      advertisement->floorplan.owner_node_id) != 0 ||
          advertisement->floorplan.owner_generation <=
              entry->owner_generation)
        return SiteDirectoryResult::CONFLICT;
    }
    if (advertisement->floorplan.owner_generation <
        entry->owner_generation)
      continue;
    if (!same_floorplan(*entry, advertisement->floorplan)) {
      *entry = advertisement->floorplan;
      changed = true;
    }
  }

  for (std::size_t slot = 0; slot < SiteGroupSummaryCache::MAX_GROUPS;
       ++slot) {
    const auto *summary = group_summaries.summary_at(slot);
    if (summary == nullptr ||
        !configuration_contains_group(configuration, summary->group_id) ||
        candidate->find_floorplan(summary->floorplan_id) == nullptr)
      continue;
    auto *entry = const_cast<GroupDirectoryEntry *>(
        candidate->find_group(summary->group_id));
    if (entry == nullptr) {
      for (auto &available : candidate->groups) {
        if (!available.occupied) {
          entry = &available;
          break;
        }
      }
      if (entry == nullptr)
        return SiteDirectoryResult::FULL;
      entry->occupied = true;
      copy_text(entry->group_id, sizeof(entry->group_id), summary->group_id);
      copy_text(entry->floorplan_id, sizeof(entry->floorplan_id),
                summary->floorplan_id);
      copy_text(entry->leader_node_id, sizeof(entry->leader_node_id),
                summary->leader_node_id);
      entry->assignment_revision = summary->assignment_revision;
      entry->member_count = summary->participants_expected;
      changed = true;
      continue;
    }
    if (summary->assignment_revision < entry->assignment_revision)
      continue;
    GroupDirectoryEntry replacement{};
    replacement.occupied = true;
    copy_text(replacement.group_id, sizeof(replacement.group_id),
              summary->group_id);
    copy_text(replacement.floorplan_id, sizeof(replacement.floorplan_id),
              summary->floorplan_id);
    copy_text(replacement.leader_node_id,
              sizeof(replacement.leader_node_id),
              summary->leader_node_id);
    replacement.assignment_revision = summary->assignment_revision;
    replacement.member_count = summary->participants_expected;
    if (summary->assignment_revision == entry->assignment_revision) {
      if (!same_group(*entry, replacement))
        return SiteDirectoryResult::CONFLICT;
      continue;
    }
    if (!same_group(*entry, replacement)) {
      *entry = replacement;
      changed = true;
    }
  }

  if (!changed)
    return SiteDirectoryResult::OK;
  candidate->committed = true;
  candidate->compatibility_mirror = false;
  candidate->legacy_single_floorplan_projection = false;
  const uint32_t base_revision =
      std::max(current.topology_revision, configuration.roster_revision);
  if (base_revision == std::numeric_limits<uint32_t>::max())
    return SiteDirectoryResult::CONFLICT;
  candidate->topology_revision = base_revision + 1;
  if (!valid_site_directory(*candidate))
    return SiteDirectoryResult::CONFLICT;
  *update_required = true;
  return SiteDirectoryResult::OK;
}

SiteDirectoryCodecResult SiteDirectoryCodec::encode(
    const SiteDirectorySnapshot &directory, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return SiteDirectoryCodecResult::INVALID_ARGUMENT;
  if (!valid_site_directory(directory))
    return SiteDirectoryCodecResult::INVALID_DIRECTORY;

  const std::size_t site_length = std::strlen(directory.site_id);
  std::size_t encoded_size = HEADER_SIZE + site_length + CHECKSUM_SIZE;
  for (const auto &entry : directory.floorplans) {
    if (entry.occupied)
      encoded_size += FLOORPLAN_HEADER_SIZE +
                      std::strlen(entry.floorplan_id) +
                      std::strlen(entry.owner_node_id);
  }
  for (const auto &entry : directory.groups) {
    if (entry.occupied)
      encoded_size += GROUP_HEADER_SIZE + std::strlen(entry.group_id) +
                      std::strlen(entry.floorplan_id) +
                      std::strlen(entry.leader_node_id);
  }
  if (encoded_size > output_capacity)
    return SiteDirectoryCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, DIRECTORY_MAGIC, sizeof(DIRECTORY_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(directory.floorplan_count());
  output[7] = static_cast<uint8_t>(directory.group_count());
  write_u32(output + 8, directory.generation);
  write_u32(output + 12, directory.topology_revision);
  output[16] = static_cast<uint8_t>(site_length);
  output[17] =
      (directory.committed ? DIRECTORY_FLAG_COMMITTED : 0) |
      (directory.compatibility_mirror
           ? DIRECTORY_FLAG_COMPATIBILITY_MIRROR
           : 0) |
      (directory.legacy_single_floorplan_projection
           ? DIRECTORY_FLAG_LEGACY_SINGLE_FLOORPLAN_PROJECTION
           : 0);
  std::memset(output + 18, 0, HEADER_SIZE - 18);

  std::size_t offset = HEADER_SIZE;
  std::memcpy(output + offset, directory.site_id, site_length);
  offset += site_length;
  for (const auto &entry : directory.floorplans) {
    if (!entry.occupied)
      continue;
    const std::size_t floorplan_length = std::strlen(entry.floorplan_id);
    const std::size_t owner_length = std::strlen(entry.owner_node_id);
    output[offset++] = entry.committed ? FLOORPLAN_FLAG_COMMITTED : 0;
    output[offset++] = static_cast<uint8_t>(floorplan_length);
    output[offset++] = static_cast<uint8_t>(owner_length);
    output[offset++] = 0;
    write_u32(output + offset, entry.owner_generation);
    offset += 4;
    write_u32(output + offset, entry.geometry_version);
    offset += 4;
    write_u32(output + offset, entry.config_hash);
    offset += 4;
    write_u32(output + offset, entry.image_hash);
    offset += 4;
    std::memcpy(output + offset, entry.floorplan_id, floorplan_length);
    offset += floorplan_length;
    std::memcpy(output + offset, entry.owner_node_id, owner_length);
    offset += owner_length;
  }
  for (const auto &entry : directory.groups) {
    if (!entry.occupied)
      continue;
    const std::size_t group_length = std::strlen(entry.group_id);
    const std::size_t floorplan_length = std::strlen(entry.floorplan_id);
    const std::size_t leader_length = std::strlen(entry.leader_node_id);
    output[offset++] = static_cast<uint8_t>(group_length);
    output[offset++] = static_cast<uint8_t>(floorplan_length);
    output[offset++] = static_cast<uint8_t>(leader_length);
    output[offset++] = entry.member_count;
    write_u32(output + offset, entry.assignment_revision);
    offset += 4;
    std::memset(output + offset, 0, 4);
    offset += 4;
    std::memcpy(output + offset, entry.group_id, group_length);
    offset += group_length;
    std::memcpy(output + offset, entry.floorplan_id, floorplan_length);
    offset += floorplan_length;
    std::memcpy(output + offset, entry.leader_node_id, leader_length);
    offset += leader_length;
  }
  write_u32(output + offset, fnv1a(output, offset));
  *output_size = offset + CHECKSUM_SIZE;
  return SiteDirectoryCodecResult::OK;
}

SiteDirectoryCodecResult SiteDirectoryCodec::decode(
    const uint8_t *data, std::size_t size,
    SiteDirectorySnapshot *directory) {
  if (data == nullptr || directory == nullptr)
    return SiteDirectoryCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF) ||
      all_bytes_are(data, size, 0x00))
    return SiteDirectoryCodecResult::EMPTY;
  if (size < HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, DIRECTORY_MAGIC, sizeof(DIRECTORY_MAGIC)) != 0)
    return SiteDirectoryCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return SiteDirectoryCodecResult::UNSUPPORTED_VERSION;

  const std::size_t floorplan_count = data[6];
  const std::size_t group_count = data[7];
  const std::size_t site_length = data[16];
  const uint8_t flags = data[17];
  if (floorplan_count > SiteDirectoryLimits::MAX_FLOORPLANS ||
      group_count > SiteDirectoryLimits::MAX_GROUPS ||
      site_length == 0 || site_length > PlatformIdentity::MAX_ID_LENGTH ||
      (flags & ~(DIRECTORY_FLAG_COMMITTED |
                 DIRECTORY_FLAG_COMPATIBILITY_MIRROR |
                 DIRECTORY_FLAG_LEGACY_SINGLE_FLOORPLAN_PROJECTION)) != 0 ||
      data[18] != 0 || data[19] != 0 || data[20] != 0 ||
      data[21] != 0 || data[22] != 0 || data[23] != 0 ||
      HEADER_SIZE + site_length + CHECKSUM_SIZE > size)
    return SiteDirectoryCodecResult::CORRUPT;

  *directory = {};
  directory->generation = read_u32(data + 8);
  directory->topology_revision = read_u32(data + 12);
  directory->committed = (flags & DIRECTORY_FLAG_COMMITTED) != 0;
  directory->compatibility_mirror =
      (flags & DIRECTORY_FLAG_COMPATIBILITY_MIRROR) != 0;
  directory->legacy_single_floorplan_projection =
      (flags & DIRECTORY_FLAG_LEGACY_SINGLE_FLOORPLAN_PROJECTION) != 0;
  std::size_t offset = HEADER_SIZE;
  std::memcpy(directory->site_id, data + offset, site_length);
  offset += site_length;

  for (std::size_t index = 0; index < floorplan_count; ++index) {
    if (offset + FLOORPLAN_HEADER_SIZE + CHECKSUM_SIZE > size)
      return SiteDirectoryCodecResult::CORRUPT;
    auto &entry = directory->floorplans[index];
    const uint8_t entry_flags = data[offset];
    const std::size_t floorplan_length = data[offset + 1];
    const std::size_t owner_length = data[offset + 2];
    if ((entry_flags & ~FLOORPLAN_FLAG_COMMITTED) != 0 ||
        data[offset + 3] != 0 || floorplan_length == 0 ||
        floorplan_length > ParticipantRecord::MAX_ID_LENGTH ||
        owner_length == 0 ||
        owner_length > ParticipantRecord::MAX_ID_LENGTH ||
        offset + FLOORPLAN_HEADER_SIZE + floorplan_length + owner_length +
                CHECKSUM_SIZE >
            size)
      return SiteDirectoryCodecResult::CORRUPT;
    entry.occupied = true;
    entry.committed = (entry_flags & FLOORPLAN_FLAG_COMMITTED) != 0;
    entry.owner_generation = read_u32(data + offset + 4);
    entry.geometry_version = read_u32(data + offset + 8);
    entry.config_hash = read_u32(data + offset + 12);
    entry.image_hash = read_u32(data + offset + 16);
    offset += FLOORPLAN_HEADER_SIZE;
    std::memcpy(entry.floorplan_id, data + offset, floorplan_length);
    offset += floorplan_length;
    std::memcpy(entry.owner_node_id, data + offset, owner_length);
    offset += owner_length;
  }

  for (std::size_t index = 0; index < group_count; ++index) {
    if (offset + GROUP_HEADER_SIZE + CHECKSUM_SIZE > size)
      return SiteDirectoryCodecResult::CORRUPT;
    auto &entry = directory->groups[index];
    const std::size_t group_length = data[offset];
    const std::size_t floorplan_length = data[offset + 1];
    const std::size_t leader_length = data[offset + 2];
    if (group_length == 0 ||
        group_length >
            ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
        floorplan_length == 0 ||
        floorplan_length > ParticipantRecord::MAX_ID_LENGTH ||
        leader_length == 0 ||
        leader_length > ParticipantRecord::MAX_ID_LENGTH ||
        data[offset + 3] == 0 ||
        data[offset + 3] >
            SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ||
        data[offset + 8] != 0 || data[offset + 9] != 0 ||
        data[offset + 10] != 0 || data[offset + 11] != 0 ||
        offset + GROUP_HEADER_SIZE + group_length + floorplan_length +
                leader_length + CHECKSUM_SIZE >
            size)
      return SiteDirectoryCodecResult::CORRUPT;
    entry.occupied = true;
    entry.member_count = data[offset + 3];
    entry.assignment_revision = read_u32(data + offset + 4);
    offset += GROUP_HEADER_SIZE;
    std::memcpy(entry.group_id, data + offset, group_length);
    offset += group_length;
    std::memcpy(entry.floorplan_id, data + offset, floorplan_length);
    offset += floorplan_length;
    std::memcpy(entry.leader_node_id, data + offset, leader_length);
    offset += leader_length;
  }

  if (offset + CHECKSUM_SIZE > size ||
      read_u32(data + offset) != fnv1a(data, offset))
    return SiteDirectoryCodecResult::CORRUPT;
  offset += CHECKSUM_SIZE;
  if (offset < size && !all_bytes_are(data + offset, size - offset, 0xFF))
    return SiteDirectoryCodecResult::CORRUPT;
  if (!valid_site_directory(*directory))
    return SiteDirectoryCodecResult::INVALID_DIRECTORY;
  return SiteDirectoryCodecResult::OK;
}

SiteDirectoryStoreResult SiteDirectoryStore::load_slot_(
    SiteDirectoryStorageBackend &backend, uint8_t slot,
    SiteDirectorySnapshot *directory) {
  if (directory == nullptr ||
      !backend.load_site_directory_slot(slot, &this->blob_))
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  const auto result = SiteDirectoryCodec::decode(
      this->blob_.bytes.data(), this->blob_.bytes.size(), directory);
  switch (result) {
    case SiteDirectoryCodecResult::OK:
      return SiteDirectoryStoreResult::LOADED;
    case SiteDirectoryCodecResult::EMPTY:
      return SiteDirectoryStoreResult::EMPTY;
    case SiteDirectoryCodecResult::UNSUPPORTED_VERSION:
      return SiteDirectoryStoreResult::UNSUPPORTED_VERSION;
    case SiteDirectoryCodecResult::INVALID_DIRECTORY:
      return SiteDirectoryStoreResult::INVALID_DIRECTORY;
    default:
      return SiteDirectoryStoreResult::CORRUPT;
  }
}

SiteDirectoryStoreResult SiteDirectoryStore::write_slot_(
    SiteDirectoryStorageBackend &backend, uint8_t slot,
    const SiteDirectorySnapshot &directory) {
  std::size_t encoded_size = 0;
  if (SiteDirectoryCodec::encode(
          directory, this->blob_.bytes.data(), this->blob_.bytes.size(),
          &encoded_size) != SiteDirectoryCodecResult::OK ||
      encoded_size == 0)
    return SiteDirectoryStoreResult::INVALID_DIRECTORY;
  if (!backend.save_site_directory_slot(slot, this->blob_) ||
      !backend.sync())
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  return SiteDirectoryStoreResult::UPDATED;
}

SiteDirectoryStoreResult SiteDirectoryStore::load(
    SiteDirectoryStorageBackend &backend,
    SiteDirectorySnapshot *directory) {
  if (directory == nullptr)
    return SiteDirectoryStoreResult::INVALID_DIRECTORY;
  this->slot_scratch_[0] = {};
  this->slot_scratch_[1] = {};
  const auto results = std::array<SiteDirectoryStoreResult, 2>{
      this->load_slot_(backend, 0, &this->slot_scratch_[0]),
      this->load_slot_(backend, 1, &this->slot_scratch_[1])};
  if (results[0] == SiteDirectoryStoreResult::STORAGE_FAILED ||
      results[1] == SiteDirectoryStoreResult::STORAGE_FAILED)
    return SiteDirectoryStoreResult::STORAGE_FAILED;

  SiteDirectoryCommitBlob commit{};
  if (!backend.load_site_directory_commit(&commit))
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  uint8_t committed_slot = 0;
  uint32_t committed_generation = 0;
  if (decode_commit(commit, &committed_slot, &committed_generation) &&
      results[committed_slot] == SiteDirectoryStoreResult::LOADED &&
      this->slot_scratch_[committed_slot].generation ==
          committed_generation) {
    *directory = this->slot_scratch_[committed_slot];
    return SiteDirectoryStoreResult::LOADED;
  }

  int selected = -1;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (results[slot] == SiteDirectoryStoreResult::LOADED &&
        (selected < 0 ||
         this->slot_scratch_[slot].generation >
             this->slot_scratch_[selected].generation))
      selected = slot;
  }
  if (selected < 0) {
    if (results[0] == SiteDirectoryStoreResult::EMPTY &&
        results[1] == SiteDirectoryStoreResult::EMPTY)
      return SiteDirectoryStoreResult::EMPTY;
    if (results[0] == SiteDirectoryStoreResult::UNSUPPORTED_VERSION ||
        results[1] == SiteDirectoryStoreResult::UNSUPPORTED_VERSION)
      return SiteDirectoryStoreResult::UNSUPPORTED_VERSION;
    if (results[0] == SiteDirectoryStoreResult::INVALID_DIRECTORY ||
        results[1] == SiteDirectoryStoreResult::INVALID_DIRECTORY)
      return SiteDirectoryStoreResult::INVALID_DIRECTORY;
    return SiteDirectoryStoreResult::CORRUPT;
  }

  encode_commit(static_cast<uint8_t>(selected),
                this->slot_scratch_[selected].generation, &commit);
  if (!backend.save_site_directory_commit(commit) || !backend.sync())
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  *directory = this->slot_scratch_[selected];
  return SiteDirectoryStoreResult::COMMIT_REPAIRED;
}

SiteDirectoryStoreResult SiteDirectoryStore::create(
    SiteDirectoryStorageBackend &backend,
    const SiteDirectorySnapshot &directory,
    SiteDirectorySnapshot *stored) {
  if (!valid_site_directory(directory))
    return SiteDirectoryStoreResult::INVALID_DIRECTORY;
  this->scratch_ = directory;
  this->scratch_.generation = 1;
  const auto write_result =
      this->write_slot_(backend, 0, this->scratch_);
  if (write_result != SiteDirectoryStoreResult::UPDATED)
    return write_result;
  SiteDirectoryCommitBlob commit{};
  encode_commit(0, this->scratch_.generation, &commit);
  if (!backend.save_site_directory_commit(commit) || !backend.sync())
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  if (stored != nullptr)
    *stored = this->scratch_;
  return SiteDirectoryStoreResult::CREATED;
}

SiteDirectoryStoreResult SiteDirectoryStore::update(
    SiteDirectoryStorageBackend &backend,
    const SiteDirectorySnapshot &directory,
    SiteDirectorySnapshot *stored) {
  const auto load_result = this->load(backend, &this->scratch_);
  if (load_result != SiteDirectoryStoreResult::LOADED &&
      load_result != SiteDirectoryStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (!valid_site_directory(directory) ||
      this->scratch_.generation == std::numeric_limits<uint32_t>::max())
    return SiteDirectoryStoreResult::INVALID_DIRECTORY;
  const uint32_t current_generation = this->scratch_.generation;
  this->scratch_ = directory;
  this->scratch_.generation = current_generation + 1;

  SiteDirectoryCommitBlob commit{};
  if (!backend.load_site_directory_commit(&commit))
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  uint8_t active_slot = 0;
  uint32_t active_generation = 0;
  if (!decode_commit(commit, &active_slot, &active_generation) ||
      active_generation != current_generation)
    return SiteDirectoryStoreResult::CORRUPT;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result =
      this->write_slot_(backend, next_slot, this->scratch_);
  if (write_result != SiteDirectoryStoreResult::UPDATED)
    return write_result;
  encode_commit(next_slot, this->scratch_.generation, &commit);
  if (!backend.save_site_directory_commit(commit) || !backend.sync())
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  if (stored != nullptr)
    *stored = this->scratch_;
  return SiteDirectoryStoreResult::UPDATED;
}

const char *site_directory_store_result_to_string(
    SiteDirectoryStoreResult result) {
  switch (result) {
    case SiteDirectoryStoreResult::EMPTY:
      return "empty";
    case SiteDirectoryStoreResult::LOADED:
      return "loaded";
    case SiteDirectoryStoreResult::CREATED:
      return "created";
    case SiteDirectoryStoreResult::UPDATED:
      return "updated";
    case SiteDirectoryStoreResult::COMMIT_REPAIRED:
      return "commit_repaired";
    case SiteDirectoryStoreResult::CORRUPT:
      return "corrupt";
    case SiteDirectoryStoreResult::UNSUPPORTED_VERSION:
      return "unsupported_version";
    case SiteDirectoryStoreResult::STORAGE_FAILED:
      return "storage_failed";
    case SiteDirectoryStoreResult::INVALID_DIRECTORY:
      return "invalid_directory";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

class RadarStorageSiteDirectoryBackend
    : public SiteDirectoryStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return this->storage_ != nullptr && this->storage_->ensure_partition();
  }

  bool load_site_directory_slot(
      uint8_t slot, SiteDirectoryStorageBlob *blob) override {
    if (slot > 1 || blob == nullptr)
      return false;
    const auto target = slot == 0 ? RadarPlatformBlob::SITE_DIRECTORY_SLOT_0
                                  : RadarPlatformBlob::SITE_DIRECTORY_SLOT_1;
    return this->storage_->read_platform_blob_raw(
        target, blob->bytes.data(), blob->bytes.size());
  }

  bool save_site_directory_slot(
      uint8_t slot, const SiteDirectoryStorageBlob &blob) override {
    if (slot > 1)
      return false;
    const auto target = slot == 0 ? RadarPlatformBlob::SITE_DIRECTORY_SLOT_0
                                  : RadarPlatformBlob::SITE_DIRECTORY_SLOT_1;
    return this->storage_->write_platform_blob(
        target, blob.bytes.data(), blob.bytes.size());
  }

  bool load_site_directory_commit(
      SiteDirectoryCommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob_raw(
                                  RadarPlatformBlob::SITE_DIRECTORY_COMMIT,
                                  blob->bytes.data(), blob->bytes.size());
  }

  bool save_site_directory_commit(
      const SiteDirectoryCommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::SITE_DIRECTORY_COMMIT, blob.bytes.data(),
        blob.bytes.size());
  }

  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32SiteDirectoryPersistence {
  RadarStorageSiteDirectoryBackend backend{};
  SiteDirectoryStore *store{nullptr};
  bool store_allocation_attempted{false};

  bool ensure_store() {
    if (this->store != nullptr)
      return true;
    if (this->store_allocation_attempted)
      return false;
    this->store_allocation_attempted = true;
#ifdef USE_PSRAM
    void *memory =
        heap_caps_calloc(1, sizeof(SiteDirectoryStore),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      ESP_LOGE(
          "site_directory",
          "Store allocation component=site_directory region=psram_failed "
          "size=%u internal_free=%u internal_largest=%u psram_free=%u "
          "psram_largest=%u",
          static_cast<unsigned>(sizeof(SiteDirectoryStore)),
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
    this->store = new (memory) SiteDirectoryStore();
    ESP_LOGI(
        "site_directory",
        "Store allocation component=site_directory region=psram size=%u "
        "internal_free=%u psram_free=%u",
        static_cast<unsigned>(sizeof(SiteDirectoryStore)),
        static_cast<unsigned>(heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
#else
    ESP_LOGE("site_directory",
             "Store allocation component=site_directory "
             "region=unavailable reason=psram_disabled size=%u",
             static_cast<unsigned>(sizeof(SiteDirectoryStore)));
    return false;
#endif
  }
};

static_assert(sizeof(Esp32SiteDirectoryPersistence) <= 32,
              "Site directory persistence shell must remain small");

Esp32SiteDirectoryPersistence &site_directory_persistence() {
  static Esp32SiteDirectoryPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_site_directory_persistence(
    SiteDirectoryStorageBackend **backend, SiteDirectoryStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = site_directory_persistence();
  if (!persistence.ensure_store()) {
    *backend = nullptr;
    *store = nullptr;
    return false;
  }
  *backend = &persistence.backend;
  *store = persistence.store;
  return true;
}

SiteDirectoryStoreResult initialize_esp32_site_directory(
    RadarStorage *storage, const SiteConfigurationSnapshot &configuration,
    SiteDirectorySnapshot *directory) {
  if (storage == nullptr || directory == nullptr)
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  auto &persistence = site_directory_persistence();
  if (!persistence.backend.initialize(storage))
    return SiteDirectoryStoreResult::STORAGE_FAILED;
  SiteDirectoryStorageBackend *backend = nullptr;
  SiteDirectoryStore *store = nullptr;
  if (!get_esp32_site_directory_persistence(&backend, &store))
    return SiteDirectoryStoreResult::STORAGE_FAILED;

  const auto load_result = store->load(*backend, directory);
  if (load_result != SiteDirectoryStoreResult::EMPTY &&
      load_result != SiteDirectoryStoreResult::LOADED &&
      load_result != SiteDirectoryStoreResult::COMMIT_REPAIRED)
    return load_result;

  bool update_required = false;
  const SiteDirectorySnapshot *current =
      load_result == SiteDirectoryStoreResult::EMPTY ? nullptr : directory;
  const uint32_t current_generation =
      current != nullptr ? current->generation : 0;
  if (prepare_site_directory_compatibility_mirror(
          configuration, current, directory, &update_required) !=
      SiteDirectoryResult::OK)
    return SiteDirectoryStoreResult::INVALID_DIRECTORY;

  if (current == nullptr)
    return store->create(*backend, *directory, directory);
  if (update_required) {
    directory->generation = current_generation;
    return store->update(*backend, *directory, directory);
  }
  return load_result;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
