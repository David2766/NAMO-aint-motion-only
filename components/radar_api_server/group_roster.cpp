#include "group_roster.h"

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

constexpr uint8_t COMMIT_MAGIC[] = {'N', 'G', 'R', 'C'};
constexpr uint16_t COMMIT_VERSION = 1;

void copy_text(char *target, std::size_t capacity, const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  target[0] = '\0';
  if (source == nullptr)
    return;
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
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

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t value = 2166136261UL;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= data[index];
    value *= 16777619UL;
  }
  return value;
}

void encode_commit(uint8_t slot, uint32_t generation,
                   GroupRosterCommitBlob *commit) {
  commit->bytes.fill(0);
  std::memcpy(commit->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(commit->bytes.data() + 4, COMMIT_VERSION);
  commit->bytes[6] = slot;
  write_u32(commit->bytes.data() + 8, generation);
  write_u32(commit->bytes.data() + 12,
            fnv1a(commit->bytes.data(), 12));
}

bool decode_commit(const GroupRosterCommitBlob &commit, uint8_t *slot,
                   uint32_t *generation) {
  if (slot == nullptr || generation == nullptr ||
      std::memcmp(commit.bytes.data(), COMMIT_MAGIC,
                  sizeof(COMMIT_MAGIC)) != 0 ||
      read_u16(commit.bytes.data() + 4) != COMMIT_VERSION ||
      commit.bytes[6] > 1 || commit.bytes[7] != 0 ||
      read_u32(commit.bytes.data() + 12) !=
          fnv1a(commit.bytes.data(), 12))
    return false;
  *slot = commit.bytes[6];
  *generation = read_u32(commit.bytes.data() + 8);
  return *generation != 0;
}

bool roster_as_configuration(const GroupRosterSnapshot &roster,
                             SiteConfigurationSnapshot *configuration) {
  if (configuration == nullptr)
    return false;
  *configuration = {};
  copy_text(configuration->site_id, sizeof(configuration->site_id),
            roster.site_id);
  copy_text(configuration->configuration_owner_node_id,
            sizeof(configuration->configuration_owner_node_id),
            roster.leader_node_id);
  configuration->authority_epoch = 1;
  configuration->generation = roster.generation == 0 ? 1 : roster.generation;
  configuration->roster_revision = roster.assignment_revision;
  configuration->roster_committed = roster.committed;
  configuration->floorplan = roster.floorplan;
  for (std::size_t index = 0; index < roster.members.size(); ++index)
    configuration->members[index] = roster.members[index];
  return true;
}

bool configuration_as_roster(const SiteConfigurationSnapshot &configuration,
                             GroupRosterSnapshot *roster) {
  if (roster == nullptr || !configuration.roster_committed ||
      !configuration.floorplan.committed)
    return false;
  *roster = {};
  copy_text(roster->site_id, sizeof(roster->site_id), configuration.site_id);
  copy_text(roster->leader_node_id, sizeof(roster->leader_node_id),
            configuration.configuration_owner_node_id);
  roster->generation = configuration.generation;
  roster->assignment_revision = configuration.roster_revision;
  roster->committed = true;
  roster->floorplan = configuration.floorplan;

  std::size_t selected = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    if (selected >= roster->members.size() ||
        !member.spatial_profile.committed)
      return false;
    if (selected == 0) {
      copy_text(roster->group_id, sizeof(roster->group_id),
                member.spatial_profile.group_id);
      copy_text(roster->space_id, sizeof(roster->space_id),
                member.spatial_profile.space_id);
    }
    roster->members[selected++] = member;
  }
  return valid_group_roster(*roster);
}

}  // namespace

const SiteConfigurationMember *GroupRosterSnapshot::find(
    const char *participant_id) const {
  if (participant_id == nullptr || participant_id[0] == '\0')
    return nullptr;
  for (const auto &member : this->members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, participant_id) == 0)
      return &member;
  }
  return nullptr;
}

std::size_t GroupRosterSnapshot::member_count() const {
  std::size_t count = 0;
  for (const auto &member : this->members) {
    if (member.occupied)
      ++count;
  }
  return count;
}

bool valid_group_roster(const GroupRosterSnapshot &roster) {
  if (!roster.committed || roster.assignment_revision == 0 ||
      !valid_text(roster.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_text(roster.group_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(roster.space_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(roster.leader_node_id,
                  ParticipantRecord::MAX_ID_LENGTH) ||
      !roster.floorplan.committed ||
      !valid_text(roster.floorplan.floorplan_id,
                  ParticipantRecord::MAX_ID_LENGTH) ||
      roster.floorplan.geometry_version == 0 ||
      roster.floorplan.config_hash == 0 || roster.floorplan.image_hash == 0 ||
      roster.member_count() == 0 ||
      roster.member_count() > GroupRosterSnapshot::MAX_MEMBERS)
    return false;

  bool leader_found = false;
  for (std::size_t index = 0; index < roster.members.size(); ++index) {
    const auto &member = roster.members[index];
    if (!member.occupied)
      continue;
    if (!valid_text(member.participant_id,
                    ParticipantRecord::MAX_ID_LENGTH) ||
        member.type != ParticipantType::NAMO_NODE ||
        member.membership != ParticipantMembership::ACTIVE ||
        !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.group_id, roster.group_id) != 0 ||
        std::strcmp(member.spatial_profile.space_id, roster.space_id) != 0 ||
        member.spatial_profile.pose_version == 0 ||
        member.spatial_profile.calibration_version == 0 ||
        !valid_spatial_transform(member.spatial_profile.transform))
      return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (roster.members[previous].occupied &&
          std::strcmp(roster.members[previous].participant_id,
                      member.participant_id) == 0)
        return false;
    }
    if (std::strcmp(member.participant_id, roster.leader_node_id) == 0)
      leader_found = member.leader_eligible;
  }
  return leader_found;
}

bool same_group_roster_payload(const GroupRosterSnapshot &left,
                               const GroupRosterSnapshot &right) {
  if (std::strcmp(left.site_id, right.site_id) != 0 ||
      std::strcmp(left.group_id, right.group_id) != 0 ||
      std::strcmp(left.space_id, right.space_id) != 0 ||
      std::strcmp(left.leader_node_id, right.leader_node_id) != 0 ||
      left.assignment_revision != right.assignment_revision ||
      left.committed != right.committed ||
      left.floorplan.committed != right.floorplan.committed ||
      std::strcmp(left.floorplan.floorplan_id,
                  right.floorplan.floorplan_id) != 0 ||
      left.floorplan.geometry_version != right.floorplan.geometry_version ||
      left.floorplan.config_hash != right.floorplan.config_hash ||
      left.floorplan.image_hash != right.floorplan.image_hash)
    return false;
  for (std::size_t index = 0; index < left.members.size(); ++index) {
    const auto &a = left.members[index];
    const auto &b = right.members[index];
    const auto &at = a.spatial_profile.transform;
    const auto &bt = b.spatial_profile.transform;
    if (a.occupied != b.occupied ||
        std::strcmp(a.participant_id, b.participant_id) != 0 ||
        a.type != b.type || a.trust != b.trust ||
        a.membership != b.membership ||
        a.leader_eligible != b.leader_eligible ||
        a.spatial_profile.committed != b.spatial_profile.committed ||
        std::strcmp(a.spatial_profile.space_id,
                    b.spatial_profile.space_id) != 0 ||
        std::strcmp(a.spatial_profile.group_id,
                    b.spatial_profile.group_id) != 0 ||
        a.spatial_profile.pose_version != b.spatial_profile.pose_version ||
        a.spatial_profile.calibration_version !=
            b.spatial_profile.calibration_version ||
        at.origin_x_mm != bt.origin_x_mm ||
        at.origin_y_mm != bt.origin_y_mm ||
        at.rotation_cdeg != bt.rotation_cdeg ||
        at.residual_mm != bt.residual_mm ||
        at.sample_count != bt.sample_count || at.valid != bt.valid)
      return false;
  }
  return true;
}

bool group_roster_update_allowed(const GroupRosterSnapshot &current,
                                 const GroupRosterSnapshot &candidate) {
  if (!valid_group_roster(current) || !valid_group_roster(candidate) ||
      same_group_roster_payload(current, candidate))
    return false;
  const bool same_scope =
      std::strcmp(current.site_id, candidate.site_id) == 0 &&
      std::strcmp(current.group_id, candidate.group_id) == 0;
  return !same_scope ||
         candidate.assignment_revision >= current.assignment_revision;
}

bool group_roster_matches_configuration_scope(
    const GroupRosterSnapshot &roster,
    const SiteConfigurationSnapshot &configuration, const char *group_id) {
  return valid_group_roster(roster) && configuration.roster_committed &&
         group_id != nullptr && group_id[0] != '\0' &&
         std::strcmp(roster.site_id, configuration.site_id) == 0 &&
         std::strcmp(roster.group_id, group_id) == 0;
}

bool group_roster_current_for_local_node(
    const GroupRosterSnapshot &roster,
    const SiteConfigurationSnapshot &configuration,
    const char *local_participant_id) {
  if (!group_roster_matches_configuration_scope(
          roster, configuration, roster.group_id) ||
      local_participant_id == nullptr || local_participant_id[0] == '\0' ||
      roster.assignment_revision != configuration.roster_revision)
    return false;
  const auto *local = configuration.find(local_participant_id);
  return local != nullptr &&
         local->membership == ParticipantMembership::ACTIVE &&
         local->type == ParticipantType::NAMO_NODE &&
         local->spatial_profile.committed &&
         std::strcmp(local->spatial_profile.group_id, roster.group_id) == 0 &&
         std::strcmp(local->spatial_profile.space_id, roster.space_id) == 0 &&
         roster.find(local_participant_id) != nullptr;
}

const SiteConfigurationMember *select_group_roster_leader(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *site_leader_node_id, const char *directory_leader_node_id,
    const char *runtime_leader_node_id, const char *stored_leader_node_id) {
  if (!configuration.roster_committed || group_id == nullptr ||
      group_id[0] == '\0')
    return nullptr;

  const auto eligible = [&](const char *participant_id) {
    const auto *member = configuration.find(participant_id);
    return member != nullptr &&
                   member->membership == ParticipantMembership::ACTIVE &&
                   member->type == ParticipantType::NAMO_NODE &&
                   member->leader_eligible &&
                   member->spatial_profile.committed &&
                   std::strcmp(member->spatial_profile.group_id, group_id) == 0 &&
                   group_leader_assignment_allowed(
                       configuration, group_id, site_leader_node_id,
                       participant_id)
               ? member
               : nullptr;
  };
  for (const char *preferred : {directory_leader_node_id,
                                runtime_leader_node_id,
                                stored_leader_node_id}) {
    const auto *member = eligible(preferred);
    if (member != nullptr)
      return member;
  }

  const SiteConfigurationMember *winner = nullptr;
  uint8_t winner_penalty = 0;
  for (const auto &member : configuration.members) {
    const auto *candidate = eligible(member.participant_id);
    if (candidate == nullptr)
      continue;
    const uint8_t penalty = group_leader_role_penalty(
        &configuration, site_leader_node_id, candidate->participant_id);
    if (winner == nullptr ||
        leader_candidate_preferred(candidate->participant_id, penalty,
                                   winner->participant_id, winner_penalty)) {
      winner = candidate;
      winner_penalty = penalty;
    }
  }
  return winner;
}

GroupRosterResult prepare_group_roster_from_site_configuration(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *leader_node_id, GroupRosterSnapshot *roster) {
  if (group_id == nullptr || leader_node_id == nullptr || roster == nullptr ||
      group_id[0] == '\0' || leader_node_id[0] == '\0' ||
      !configuration.roster_committed || !configuration.floorplan.committed)
    return GroupRosterResult::INVALID_ARGUMENT;

  *roster = {};
  auto &candidate = *roster;
  copy_text(candidate.site_id, sizeof(candidate.site_id),
            configuration.site_id);
  copy_text(candidate.group_id, sizeof(candidate.group_id), group_id);
  copy_text(candidate.leader_node_id, sizeof(candidate.leader_node_id),
            leader_node_id);
  candidate.assignment_revision = configuration.roster_revision;
  candidate.committed = true;
  candidate.floorplan = configuration.floorplan;

  std::size_t count = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.group_id, group_id) != 0)
      continue;
    if (count >= candidate.members.size()) {
      *roster = {};
      return GroupRosterResult::FULL;
    }
    if (count == 0)
      copy_text(candidate.space_id, sizeof(candidate.space_id),
                member.spatial_profile.space_id);
    else if (std::strcmp(candidate.space_id,
                         member.spatial_profile.space_id) != 0) {
      *roster = {};
      return GroupRosterResult::INVALID_ROSTER;
    }
    candidate.members[count] = member;
    candidate.members[count].spatial_profile.transform.residual_mm = 0;
    candidate.members[count].spatial_profile.transform.sample_count = 1;
    candidate.members[count].spatial_profile.transform.reserved = 0;
    ++count;
  }
  if (count == 0) {
    *roster = {};
    return GroupRosterResult::GROUP_NOT_FOUND;
  }
  std::sort(candidate.members.begin(), candidate.members.begin() + count,
            [](const SiteConfigurationMember &left,
               const SiteConfigurationMember &right) {
              return std::strcmp(left.participant_id,
                                 right.participant_id) < 0;
            });
  if (candidate.find(leader_node_id) == nullptr) {
    *roster = {};
    return GroupRosterResult::LEADER_NOT_FOUND;
  }
  if (!valid_group_roster(candidate)) {
    *roster = {};
    return GroupRosterResult::INVALID_ROSTER;
  }
  return GroupRosterResult::OK;
}

const char *group_roster_result_to_string(GroupRosterResult result) {
  switch (result) {
    case GroupRosterResult::OK: return "ok";
    case GroupRosterResult::INVALID_ARGUMENT: return "invalid_argument";
    case GroupRosterResult::INVALID_ROSTER: return "invalid_roster";
    case GroupRosterResult::GROUP_NOT_FOUND: return "group_not_found";
    case GroupRosterResult::LEADER_NOT_FOUND: return "leader_not_found";
    case GroupRosterResult::FULL: return "group_full";
  }
  return "invalid_argument";
}

GroupRosterCodecResult GroupRosterCodec::encode(
    const GroupRosterSnapshot &roster, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size,
    SiteConfigurationSnapshot *workspace) {
  if (output == nullptr || output_size == nullptr || workspace == nullptr)
    return GroupRosterCodecResult::INVALID_ARGUMENT;
  if (!valid_group_roster(roster))
    return GroupRosterCodecResult::INVALID_ROSTER;
  if (!roster_as_configuration(roster, workspace))
    return GroupRosterCodecResult::INVALID_ARGUMENT;
  const auto result = SiteConfigurationCodec::encode(
      *workspace, output, output_capacity, output_size);
  if (result == SiteConfigurationCodecResult::OK)
    return GroupRosterCodecResult::OK;
  return result == SiteConfigurationCodecResult::INVALID_CONFIGURATION
             ? GroupRosterCodecResult::INVALID_ROSTER
             : GroupRosterCodecResult::INVALID_ARGUMENT;
}

GroupRosterCodecResult GroupRosterCodec::decode(const uint8_t *data,
                                                std::size_t size,
                                                GroupRosterSnapshot *roster,
                                                SiteConfigurationSnapshot *workspace) {
  if (data == nullptr || roster == nullptr || workspace == nullptr)
    return GroupRosterCodecResult::INVALID_ARGUMENT;
  *workspace = {};
  const auto result = SiteConfigurationCodec::decode(data, size, workspace);
  if (result == SiteConfigurationCodecResult::EMPTY)
    return GroupRosterCodecResult::EMPTY;
  if (result == SiteConfigurationCodecResult::UNSUPPORTED_VERSION)
    return GroupRosterCodecResult::UNSUPPORTED_VERSION;
  if (result != SiteConfigurationCodecResult::OK)
    return GroupRosterCodecResult::CORRUPT;
  return configuration_as_roster(*workspace, roster)
             ? GroupRosterCodecResult::OK
             : GroupRosterCodecResult::INVALID_ROSTER;
}

GroupRosterStoreResult GroupRosterStore::load_slot_(
    GroupRosterStorageBackend &backend, uint8_t slot,
    GroupRosterSnapshot *roster) {
  if (roster == nullptr || !backend.load_group_roster_slot(slot, &this->blob_))
    return GroupRosterStoreResult::STORAGE_FAILED;
  const auto result = GroupRosterCodec::decode(
      this->blob_.bytes.data(), this->blob_.bytes.size(), roster,
      &this->codec_scratch_);
  if (result == GroupRosterCodecResult::OK)
    return GroupRosterStoreResult::LOADED;
  if (result == GroupRosterCodecResult::EMPTY)
    return GroupRosterStoreResult::EMPTY;
  if (result == GroupRosterCodecResult::UNSUPPORTED_VERSION)
    return GroupRosterStoreResult::UNSUPPORTED_VERSION;
  if (result == GroupRosterCodecResult::INVALID_ROSTER)
    return GroupRosterStoreResult::INVALID_ROSTER;
  return GroupRosterStoreResult::CORRUPT;
}

GroupRosterStoreResult GroupRosterStore::write_slot_(
    GroupRosterStorageBackend &backend, uint8_t slot,
    const GroupRosterSnapshot &roster) {
  std::size_t encoded_size = 0;
  if (GroupRosterCodec::encode(roster, this->blob_.bytes.data(),
                               this->blob_.bytes.size(), &encoded_size,
                               &this->codec_scratch_) !=
          GroupRosterCodecResult::OK ||
      encoded_size == 0)
    return GroupRosterStoreResult::INVALID_ROSTER;
  if (!backend.save_group_roster_slot(slot, this->blob_) || !backend.sync())
    return GroupRosterStoreResult::STORAGE_FAILED;
  return GroupRosterStoreResult::UPDATED;
}

GroupRosterStoreResult GroupRosterStore::load(
    GroupRosterStorageBackend &backend, GroupRosterSnapshot *roster) {
  if (roster == nullptr)
    return GroupRosterStoreResult::INVALID_ROSTER;
  this->slot_scratch_[0] = {};
  this->slot_scratch_[1] = {};
  const std::array<GroupRosterStoreResult, 2> results{
      this->load_slot_(backend, 0, &this->slot_scratch_[0]),
      this->load_slot_(backend, 1, &this->slot_scratch_[1])};
  if (results[0] == GroupRosterStoreResult::STORAGE_FAILED ||
      results[1] == GroupRosterStoreResult::STORAGE_FAILED)
    return GroupRosterStoreResult::STORAGE_FAILED;

  GroupRosterCommitBlob commit{};
  if (!backend.load_group_roster_commit(&commit))
    return GroupRosterStoreResult::STORAGE_FAILED;
  uint8_t committed_slot = 0;
  uint32_t committed_generation = 0;
  if (decode_commit(commit, &committed_slot, &committed_generation) &&
      results[committed_slot] == GroupRosterStoreResult::LOADED &&
      this->slot_scratch_[committed_slot].generation ==
          committed_generation) {
    *roster = this->slot_scratch_[committed_slot];
    return GroupRosterStoreResult::LOADED;
  }

  int selected = -1;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (results[slot] == GroupRosterStoreResult::LOADED &&
        (selected < 0 || this->slot_scratch_[slot].generation >
                             this->slot_scratch_[selected].generation))
      selected = slot;
  }
  if (selected < 0) {
    if (results[0] == GroupRosterStoreResult::EMPTY &&
        results[1] == GroupRosterStoreResult::EMPTY)
      return GroupRosterStoreResult::EMPTY;
    if (results[0] == GroupRosterStoreResult::UNSUPPORTED_VERSION ||
        results[1] == GroupRosterStoreResult::UNSUPPORTED_VERSION)
      return GroupRosterStoreResult::UNSUPPORTED_VERSION;
    if (results[0] == GroupRosterStoreResult::INVALID_ROSTER ||
        results[1] == GroupRosterStoreResult::INVALID_ROSTER)
      return GroupRosterStoreResult::INVALID_ROSTER;
    return GroupRosterStoreResult::CORRUPT;
  }
  encode_commit(static_cast<uint8_t>(selected),
                this->slot_scratch_[selected].generation, &commit);
  if (!backend.save_group_roster_commit(commit) || !backend.sync())
    return GroupRosterStoreResult::STORAGE_FAILED;
  *roster = this->slot_scratch_[selected];
  return GroupRosterStoreResult::COMMIT_REPAIRED;
}

GroupRosterStoreResult GroupRosterStore::create(
    GroupRosterStorageBackend &backend, const GroupRosterSnapshot &roster,
    GroupRosterSnapshot *stored) {
  if (!valid_group_roster(roster))
    return GroupRosterStoreResult::INVALID_ROSTER;
  this->scratch_ = roster;
  this->scratch_.generation = 1;
  const auto write_result = this->write_slot_(backend, 0, this->scratch_);
  if (write_result != GroupRosterStoreResult::UPDATED)
    return write_result;
  GroupRosterCommitBlob commit{};
  encode_commit(0, this->scratch_.generation, &commit);
  if (!backend.save_group_roster_commit(commit) || !backend.sync())
    return GroupRosterStoreResult::STORAGE_FAILED;
  if (stored != nullptr)
    *stored = this->scratch_;
  return GroupRosterStoreResult::CREATED;
}

GroupRosterStoreResult GroupRosterStore::update(
    GroupRosterStorageBackend &backend, const GroupRosterSnapshot &roster,
    GroupRosterSnapshot *stored) {
  const auto load_result = this->load(backend, &this->scratch_);
  if (load_result != GroupRosterStoreResult::LOADED &&
      load_result != GroupRosterStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (!valid_group_roster(roster) ||
      this->scratch_.generation == std::numeric_limits<uint32_t>::max())
    return GroupRosterStoreResult::INVALID_ROSTER;
  if (same_group_roster_payload(this->scratch_, roster)) {
    if (stored != nullptr)
      *stored = this->scratch_;
    return GroupRosterStoreResult::LOADED;
  }
  const uint32_t current_generation = this->scratch_.generation;
  this->scratch_ = roster;
  this->scratch_.generation = current_generation + 1;

  GroupRosterCommitBlob commit{};
  uint8_t active_slot = 0;
  uint32_t active_generation = 0;
  if (!backend.load_group_roster_commit(&commit) ||
      !decode_commit(commit, &active_slot, &active_generation) ||
      active_generation != current_generation)
    return GroupRosterStoreResult::CORRUPT;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result =
      this->write_slot_(backend, next_slot, this->scratch_);
  if (write_result != GroupRosterStoreResult::UPDATED)
    return write_result;
  encode_commit(next_slot, this->scratch_.generation, &commit);
  if (!backend.save_group_roster_commit(commit) || !backend.sync())
    return GroupRosterStoreResult::STORAGE_FAILED;
  if (stored != nullptr)
    *stored = this->scratch_;
  return GroupRosterStoreResult::UPDATED;
}

GroupRosterStoreResult GroupRosterStore::load_or_migrate(
    GroupRosterStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *leader_node_id, GroupRosterSnapshot *roster) {
  const auto loaded = this->load(backend, roster);
  if (loaded != GroupRosterStoreResult::EMPTY)
    return loaded;
  this->scratch_ = {};
  if (prepare_group_roster_from_site_configuration(
          configuration, group_id, leader_node_id, &this->scratch_) !=
      GroupRosterResult::OK)
    return GroupRosterStoreResult::EMPTY;
  const auto created = this->create(backend, this->scratch_, roster);
  return created == GroupRosterStoreResult::CREATED
             ? GroupRosterStoreResult::MIGRATED
             : created;
}

const char *group_roster_store_result_to_string(
    GroupRosterStoreResult result) {
  switch (result) {
    case GroupRosterStoreResult::EMPTY: return "empty";
    case GroupRosterStoreResult::LOADED: return "loaded";
    case GroupRosterStoreResult::CREATED: return "created";
    case GroupRosterStoreResult::MIGRATED: return "migrated";
    case GroupRosterStoreResult::UPDATED: return "updated";
    case GroupRosterStoreResult::COMMIT_REPAIRED: return "commit_repaired";
    case GroupRosterStoreResult::CORRUPT: return "corrupt";
    case GroupRosterStoreResult::UNSUPPORTED_VERSION:
      return "unsupported_version";
    case GroupRosterStoreResult::STORAGE_FAILED: return "storage_failed";
    case GroupRosterStoreResult::INVALID_ROSTER: return "invalid_roster";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

class RadarStorageGroupRosterBackend : public GroupRosterStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return storage != nullptr && storage->ensure_partition() &&
           storage->prepare_group_roster_storage();
  }

  bool load_group_roster_slot(uint8_t slot,
                              GroupRosterStorageBlob *blob) override {
    if (slot > 1 || blob == nullptr)
      return false;
    const auto target = slot == 0 ? RadarPlatformBlob::GROUP_ROSTER_SLOT_0
                                  : RadarPlatformBlob::GROUP_ROSTER_SLOT_1;
    return this->storage_->read_platform_blob_raw(
        target, blob->bytes.data(), blob->bytes.size());
  }

  bool save_group_roster_slot(
      uint8_t slot, const GroupRosterStorageBlob &blob) override {
    if (slot > 1)
      return false;
    const auto target = slot == 0 ? RadarPlatformBlob::GROUP_ROSTER_SLOT_0
                                  : RadarPlatformBlob::GROUP_ROSTER_SLOT_1;
    return this->storage_->write_platform_blob(
        target, blob.bytes.data(), blob.bytes.size());
  }

  bool load_group_roster_commit(GroupRosterCommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob_raw(
                                  RadarPlatformBlob::GROUP_ROSTER_COMMIT,
                                  blob->bytes.data(), blob->bytes.size());
  }

  bool save_group_roster_commit(
      const GroupRosterCommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::GROUP_ROSTER_COMMIT, blob.bytes.data(),
        blob.bytes.size());
  }

  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32GroupRosterPersistence {
  RadarStorageGroupRosterBackend backend{};
  GroupRosterStore *store{nullptr};
  bool store_allocation_attempted{false};

  bool ensure_store() {
    if (this->store != nullptr)
      return true;
    if (this->store_allocation_attempted)
      return false;
    this->store_allocation_attempted = true;
#ifdef USE_PSRAM
    void *memory = heap_caps_calloc(1, sizeof(GroupRosterStore),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      ESP_LOGE("group_roster",
               "Store allocation component=group_roster region=psram_failed "
               "size=%u internal_free=%u psram_free=%u",
               static_cast<unsigned>(sizeof(GroupRosterStore)),
               static_cast<unsigned>(heap_caps_get_free_size(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      return false;
    }
    this->store = new (memory) GroupRosterStore();
    ESP_LOGI("group_roster",
             "Store allocation component=group_roster region=psram size=%u "
             "internal_free=%u psram_free=%u",
             static_cast<unsigned>(sizeof(GroupRosterStore)),
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
#else
    ESP_LOGE("group_roster",
             "Store allocation component=group_roster region=unavailable "
             "reason=psram_disabled size=%u",
             static_cast<unsigned>(sizeof(GroupRosterStore)));
    return false;
#endif
  }
};

Esp32GroupRosterPersistence &group_roster_persistence() {
  static Esp32GroupRosterPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_group_roster_persistence(GroupRosterStorageBackend **backend,
                                        GroupRosterStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = group_roster_persistence();
  if (!persistence.ensure_store()) {
    *backend = nullptr;
    *store = nullptr;
    return false;
  }
  *backend = &persistence.backend;
  *store = persistence.store;
  return true;
}

GroupRosterStoreResult initialize_esp32_group_roster(
    RadarStorage *storage, const SiteConfigurationSnapshot &configuration,
    const char *group_id, const char *leader_node_id,
    GroupRosterSnapshot *roster) {
  if (storage == nullptr || roster == nullptr)
    return GroupRosterStoreResult::STORAGE_FAILED;
  auto &persistence = group_roster_persistence();
  if (!persistence.backend.initialize(storage))
    return GroupRosterStoreResult::STORAGE_FAILED;
  GroupRosterStorageBackend *backend = nullptr;
  GroupRosterStore *store = nullptr;
  if (!get_esp32_group_roster_persistence(&backend, &store))
    return GroupRosterStoreResult::STORAGE_FAILED;
  return store->load_or_migrate(*backend, configuration, group_id,
                                leader_node_id, roster);
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
