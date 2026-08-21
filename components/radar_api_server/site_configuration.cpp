#include "site_configuration.h"

#include "group_membership.h"
#include "participant_credentials.h"
#include "site_directory.h"

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

constexpr uint8_t CONFIG_MAGIC[] = {'N', 'A', 'M', 'C'};
constexpr uint8_t COMMIT_MAGIC[] = {'N', 'M', 'C', 'C'};
constexpr uint16_t COMMIT_VERSION = 1;
constexpr uint16_t LEGACY_STORAGE_VERSION = 1;
constexpr uint16_t SPATIAL_PROFILE_STORAGE_VERSION = 2;
constexpr uint16_t GROUP_ASSIGNMENT_STORAGE_VERSION = 3;
constexpr std::size_t LEGACY_HEADER_SIZE = 32;
constexpr std::size_t HEADER_SIZE = 36;
constexpr std::size_t RECORD_HEADER_SIZE = 5;
constexpr std::size_t SPATIAL_PROFILE_V2_SIZE = 18;
constexpr std::size_t SPATIAL_PROFILE_V3_HEADER_SIZE = 2;
constexpr std::size_t CHECKSUM_SIZE = 4;
constexpr uint8_t FLAG_ROSTER_COMMITTED = 0x01;
constexpr uint8_t FLAG_FLOORPLAN_COMMITTED = 0x02;
constexpr uint8_t MEMBER_FLAG_LEADER_ELIGIBLE = 0x01;
constexpr uint8_t MEMBER_FLAG_SPATIAL_PROFILE = 0x02;

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

int16_t read_i16(const uint8_t *data) {
  return static_cast<int16_t>(read_u16(data));
}

int32_t read_i32(const uint8_t *data) {
  return static_cast<int32_t>(read_u32(data));
}

void write_i16(uint8_t *data, int16_t value) {
  write_u16(data, static_cast<uint16_t>(value));
}

void write_i32(uint8_t *data, int32_t value) {
  write_u32(data, static_cast<uint32_t>(value));
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

void digest_bytes(uint64_t *hash, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (std::size_t index = 0; index < size; ++index) {
    *hash ^= bytes[index];
    *hash *= 1099511628211ULL;
  }
}

void digest_u8(uint64_t *hash, uint8_t value) {
  digest_bytes(hash, &value, sizeof(value));
}

void digest_u32(uint64_t *hash, uint32_t value) {
  uint8_t encoded[4]{};
  write_u32(encoded, value);
  digest_bytes(hash, encoded, sizeof(encoded));
}

void digest_i32(uint64_t *hash, int32_t value) {
  digest_u32(hash, static_cast<uint32_t>(value));
}

void digest_i16(uint64_t *hash, int16_t value) {
  uint8_t encoded[2]{};
  write_i16(encoded, value);
  digest_bytes(hash, encoded, sizeof(encoded));
}

void digest_text(uint64_t *hash, const char *value) {
  const std::size_t length = value != nullptr ? std::strlen(value) : 0;
  digest_u32(hash, static_cast<uint32_t>(length));
  if (length > 0)
    digest_bytes(hash, value, length);
}

bool all_bytes_are(const uint8_t *data, std::size_t size, uint8_t value) {
  for (std::size_t index = 0; index < size; ++index) {
    if (data[index] != value)
      return false;
  }
  return true;
}

bool valid_identifier(const char *value, std::size_t max_length,
                      bool allow_empty = false, std::size_t *length_out = nullptr) {
  if (value == nullptr || (value[0] == '\0' && !allow_empty))
    return false;
  std::size_t length = 0;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (++length > max_length)
      return false;
    const char character = *cursor;
    const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9');
    if (!alpha_numeric &&
        (cursor == value || (character != '.' && character != '_' &&
                             character != ':' && character != '-')))
      return false;
  }
  if (length_out != nullptr)
    *length_out = length;
  return allow_empty || length > 0;
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

bool valid_member(const SiteConfigurationMember &member) {
  if (!member.occupied ||
      !valid_identifier(member.participant_id,
                        ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_type(member.type) || !valid_trust(member.trust) ||
      !valid_membership(member.membership))
    return false;
  if (member.leader_eligible &&
      (member.type != ParticipantType::NAMO_NODE ||
       member.membership != ParticipantMembership::ACTIVE))
    return false;
  if (member.spatial_profile.committed) {
    const bool has_space = member.spatial_profile.space_id[0] != '\0';
    const bool has_group = member.spatial_profile.group_id[0] != '\0';
    if (member.type != ParticipantType::NAMO_NODE ||
        has_space != has_group ||
        (has_space &&
         (!valid_identifier(
              member.spatial_profile.space_id,
              ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
          !valid_identifier(
              member.spatial_profile.group_id,
              ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH))) ||
        member.spatial_profile.pose_version == 0 ||
        member.spatial_profile.calibration_version == 0 ||
        !valid_spatial_transform(member.spatial_profile.transform))
      return false;
  } else if (member.spatial_profile.pose_version != 0 ||
             member.spatial_profile.calibration_version != 0 ||
             member.spatial_profile.space_id[0] != '\0' ||
             member.spatial_profile.group_id[0] != '\0' ||
             member.spatial_profile.transform.valid) {
    return false;
  }
  return true;
}

bool member_can_receive_site_replica(
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id) {
  const auto *member = configuration.find(participant_id);
  return member != nullptr && member->type == ParticipantType::NAMO_NODE &&
         member->membership != ParticipantMembership::REVOKED;
}

bool valid_configuration(const SiteConfigurationSnapshot &snapshot) {
  if (!valid_identifier(snapshot.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      snapshot.authority_epoch == 0 || snapshot.generation == 0 ||
      snapshot.roster_revision == 0)
    return false;
  const bool owner_resolved = snapshot.configuration_owner_node_id[0] != '\0';
  if (!valid_identifier(snapshot.configuration_owner_node_id,
                        ParticipantRecord::MAX_ID_LENGTH, true) ||
      (snapshot.roster_committed && !owner_resolved))
    return false;

  std::size_t count = 0;
  for (std::size_t slot = 0; slot < snapshot.members.size(); ++slot) {
    const auto &member = snapshot.members[slot];
    if (!member.occupied)
      continue;
    if (!valid_member(member))
      return false;
    ++count;
    for (std::size_t other = slot + 1; other < snapshot.members.size(); ++other) {
      if (snapshot.members[other].occupied &&
          std::strcmp(member.participant_id,
                      snapshot.members[other].participant_id) == 0)
        return false;
    }
  }
  if (count == 0)
    return false;
  if (owner_resolved) {
    const auto *owner = snapshot.find(snapshot.configuration_owner_node_id);
    if (owner == nullptr || owner->type != ParticipantType::NAMO_NODE ||
        owner->membership != ParticipantMembership::ACTIVE ||
        !owner->leader_eligible)
      return false;
  }
  if (snapshot.floorplan.committed &&
      (!valid_identifier(snapshot.floorplan.floorplan_id,
                         ParticipantRecord::MAX_ID_LENGTH) ||
       snapshot.floorplan.geometry_version == 0))
    return false;
  if (!snapshot.floorplan.committed &&
      (snapshot.floorplan.floorplan_id[0] != '\0' ||
       snapshot.floorplan.geometry_version != 0 ||
       snapshot.floorplan.config_hash != 0 ||
       snapshot.floorplan.image_hash != 0))
    return false;
  if (!snapshot.floorplan.committed) {
    for (const auto &member : snapshot.members) {
      if (member.occupied && member.spatial_profile.committed)
        return false;
    }
  }
  return true;
}

bool same_transform(const SpatialTransform2D &left,
                    const SpatialTransform2D &right) {
  return left.origin_x_mm == right.origin_x_mm &&
         left.origin_y_mm == right.origin_y_mm &&
         left.rotation_cdeg == right.rotation_cdeg &&
         left.residual_mm == right.residual_mm &&
         left.sample_count == right.sample_count &&
         left.valid == right.valid;
}

bool same_spatial_profile(const SiteSpatialProfile &left,
                          const SiteSpatialProfile &right) {
  return left.committed == right.committed &&
         std::strcmp(left.space_id, right.space_id) == 0 &&
         std::strcmp(left.group_id, right.group_id) == 0 &&
         left.pose_version == right.pose_version &&
         left.calibration_version == right.calibration_version &&
         same_transform(left.transform, right.transform);
}

bool same_configuration(const SiteConfigurationSnapshot &left,
                        const SiteConfigurationSnapshot &right) {
  if (std::strcmp(left.site_id, right.site_id) != 0 ||
      std::strcmp(left.configuration_owner_node_id,
                  right.configuration_owner_node_id) != 0 ||
      left.authority_epoch != right.authority_epoch ||
      left.generation != right.generation ||
      left.roster_revision != right.roster_revision ||
      left.roster_committed != right.roster_committed ||
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
    if (a.occupied != b.occupied ||
        std::strcmp(a.participant_id, b.participant_id) != 0 ||
        a.type != b.type || a.trust != b.trust ||
        a.membership != b.membership ||
        a.leader_eligible != b.leader_eligible ||
        !same_spatial_profile(a.spatial_profile, b.spatial_profile))
      return false;
  }
  return true;
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

void sort_members(SiteConfigurationSnapshot *snapshot) {
  std::size_t occupied = 0;
  for (std::size_t index = 0; index < snapshot->members.size(); ++index) {
    if (!snapshot->members[index].occupied)
      continue;
    if (occupied != index)
      snapshot->members[occupied] = snapshot->members[index];
    ++occupied;
  }
  for (std::size_t index = occupied; index < snapshot->members.size(); ++index)
    snapshot->members[index] = {};

  for (std::size_t index = 1; index < occupied; ++index) {
    SiteConfigurationMember current = snapshot->members[index];
    std::size_t position = index;
    while (position > 0 &&
           std::strcmp(snapshot->members[position - 1].participant_id,
                       current.participant_id) > 0) {
      snapshot->members[position] = snapshot->members[position - 1];
      --position;
    }
    snapshot->members[position] = current;
  }
}

void preserve_spatial_profile(const SiteConfigurationSnapshot &current,
                              SiteConfigurationMember *member) {
  if (member == nullptr)
    return;
  const auto *existing = current.find(member->participant_id);
  if (existing != nullptr)
    member->spatial_profile = existing->spatial_profile;
}

ParticipantTrust canonical_namo_trust(ParticipantType type,
                                      ParticipantTrust trust) {
  return type == ParticipantType::NAMO_NODE &&
                 trust == ParticipantTrust::OFFICIAL_VERIFIED
             ? ParticipantTrust::COMMUNITY_UNVERIFIED
             : trust;
}

void clear_spatial_profiles(SiteConfigurationSnapshot *snapshot) {
  if (snapshot == nullptr)
    return;
  for (auto &member : snapshot->members) {
    if (member.occupied)
      member.spatial_profile = {};
  }
}

SiteConfigurationResult build_local_owned_roster(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  SiteConfigurationSnapshot next = current;
  const bool owner_changed =
      current.configuration_owner_node_id[0] != '\0' &&
      std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0;
  if (owner_changed &&
      current.authority_epoch == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  copy_text(next.configuration_owner_node_id,
            sizeof(next.configuration_owner_node_id), identity.node_id);
  if (owner_changed)
    ++next.authority_epoch;
  next.members = {};

  auto &local = next.members[0];
  local.occupied = true;
  copy_text(local.participant_id, sizeof(local.participant_id),
            identity.node_id);
  local.type = ParticipantType::NAMO_NODE;
  local.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  local.membership = ParticipantMembership::ACTIVE;
  local.leader_eligible = true;
  preserve_spatial_profile(current, &local);

  std::size_t next_slot = 1;
  if (!owner_changed) {
    for (const auto &existing : current.members) {
      if (!existing.occupied ||
          std::strcmp(existing.participant_id, identity.node_id) == 0 ||
          existing.membership == ParticipantMembership::REVOKED)
        continue;
      const auto *direct = participants.find(existing.participant_id);
      if (direct != nullptr &&
          direct->membership == ParticipantMembership::REVOKED)
        continue;
      if (next_slot >= next.members.size())
        return SiteConfigurationResult::FULL;
      auto &member = next.members[next_slot++];
      if (direct == nullptr) {
        member = existing;
        member.trust = canonical_namo_trust(member.type, member.trust);
        continue;
      }
      member.occupied = true;
      copy_text(member.participant_id, sizeof(member.participant_id),
                direct->participant_id);
      member.type = direct->type;
      member.trust = canonical_namo_trust(direct->type, direct->trust);
      member.membership = direct->membership;
      member.leader_eligible = direct->leader_eligible;
      member.spatial_profile = existing.spatial_profile;
    }
  }

  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    const auto *existing =
        participant != nullptr ? current.find(participant->participant_id)
                               : nullptr;
    if (participant == nullptr ||
        participant->membership == ParticipantMembership::REVOKED ||
        (!owner_changed && existing != nullptr &&
         existing->membership != ParticipantMembership::REVOKED))
      continue;
    if (next_slot >= next.members.size())
      return SiteConfigurationResult::FULL;
    auto &member = next.members[next_slot++];
    member.occupied = true;
    copy_text(member.participant_id, sizeof(member.participant_id),
              participant->participant_id);
    member.type = participant->type;
    member.trust =
        canonical_namo_trust(participant->type, participant->trust);
    member.membership = participant->membership;
    member.leader_eligible = participant->leader_eligible;
    preserve_spatial_profile(current, &member);
  }

  sort_members(&next);
  next.roster_committed = true;
  if (same_configuration(current, next)) {
    *updated = current;
    return SiteConfigurationResult::OK;
  }
  if (current.roster_revision == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  ++next.roster_revision;
  if (!valid_configuration(next))
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  *updated = next;
  return SiteConfigurationResult::OK;
}

void encode_commit(uint8_t slot, uint32_t generation,
                   SiteConfigurationCommitBlob *blob) {
  blob->bytes.fill(0xFF);
  std::memcpy(blob->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(blob->bytes.data() + 4, COMMIT_VERSION);
  blob->bytes[6] = slot;
  blob->bytes[7] = 0;
  write_u32(blob->bytes.data() + 8, generation);
  write_u32(blob->bytes.data() + 12, fnv1a(blob->bytes.data(), 12));
}

SiteConfigurationStoreResult decode_commit(
    const SiteConfigurationCommitBlob &blob, uint8_t *slot,
    uint32_t *generation) {
  if (all_bytes_are(blob.bytes.data(), blob.bytes.size(), 0xFF))
    return SiteConfigurationStoreResult::EMPTY;
  if (std::memcmp(blob.bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0 ||
      read_u16(blob.bytes.data() + 4) != COMMIT_VERSION || blob.bytes[6] > 1 ||
      blob.bytes[7] != 0 || read_u32(blob.bytes.data() + 8) == 0 ||
      read_u32(blob.bytes.data() + 12) != fnv1a(blob.bytes.data(), 12))
    return SiteConfigurationStoreResult::CORRUPT;
  *slot = blob.bytes[6];
  *generation = read_u32(blob.bytes.data() + 8);
  return SiteConfigurationStoreResult::LOADED;
}

}  // namespace

const SiteConfigurationMember *SiteConfigurationSnapshot::find(
    const char *participant_id) const {
  if (participant_id == nullptr)
    return nullptr;
  for (const auto &member : this->members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, participant_id) == 0)
      return &member;
  }
  return nullptr;
}

std::size_t SiteConfigurationSnapshot::member_count() const {
  std::size_t count = 0;
  for (const auto &member : this->members) {
    if (member.occupied)
      ++count;
  }
  return count;
}

std::size_t SiteConfigurationSnapshot::active_member_count() const {
  std::size_t count = 0;
  for (const auto &member : this->members) {
    if (member.occupied && member.membership == ParticipantMembership::ACTIVE)
      ++count;
  }
  return count;
}

AutomaticSpatialGroupResult resolve_automatic_spatial_group(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *peer_participant_id, const char *seed_space_id,
    const char *seed_group_id,
    AutomaticSpatialGroupDecision *decision) {
  if (decision == nullptr || !valid_configuration(configuration) ||
      !valid_identifier(identity.node_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_identifier(peer_participant_id,
                        ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_identifier(
          seed_space_id,
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_identifier(
          seed_group_id,
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      std::strcmp(identity.site_id, configuration.site_id) != 0 ||
      std::strcmp(identity.node_id, peer_participant_id) == 0)
    return AutomaticSpatialGroupResult::INVALID_ARGUMENT;

  *decision = AutomaticSpatialGroupDecision{};
  const auto *local = configuration.find(identity.node_id);
  const auto *peer = configuration.find(peer_participant_id);
  if (local == nullptr || peer == nullptr ||
      local->type != ParticipantType::NAMO_NODE ||
      peer->type != ParticipantType::NAMO_NODE ||
      local->membership == ParticipantMembership::REVOKED ||
      peer->membership == ParticipantMembership::REVOKED)
    return AutomaticSpatialGroupResult::NOT_READY;

  const char *target_group = seed_group_id;
  bool existing_group_selected = false;
  if (peer->spatial_profile.committed &&
      std::strcmp(peer->spatial_profile.space_id, seed_space_id) == 0) {
    target_group = peer->spatial_profile.group_id;
    existing_group_selected = true;
  }
  if (!existing_group_selected) {
    for (const auto &member : configuration.members) {
      if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
          member.membership == ParticipantMembership::REVOKED ||
          !member.spatial_profile.committed ||
          std::strcmp(member.spatial_profile.space_id, seed_space_id) != 0)
        continue;
      target_group = member.spatial_profile.group_id;
      existing_group_selected = true;
      break;
    }
  }
  const auto group_crosses_space = [&](const char *group_id) {
    for (const auto &member : configuration.members) {
      if (!member.occupied || !member.spatial_profile.committed ||
          std::strcmp(member.spatial_profile.group_id, group_id) != 0)
        continue;
      if (std::strcmp(member.spatial_profile.space_id, seed_space_id) != 0)
        return true;
    }
    return false;
  };
  if (group_crosses_space(target_group)) {
    target_group = group_crosses_space(seed_group_id) ? seed_space_id
                                                       : seed_group_id;
  }

  copy_text(decision->space_id, sizeof(decision->space_id), seed_space_id);
  copy_text(decision->group_id, sizeof(decision->group_id), target_group);

  bool peer_counted = false;
  uint8_t participant_count = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
        member.membership == ParticipantMembership::REVOKED ||
        !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.space_id, seed_space_id) != 0)
      continue;
    if (participant_count != UINT8_MAX)
      ++participant_count;
    peer_counted = peer_counted ||
                   std::strcmp(member.participant_id, peer_participant_id) == 0;
    decision->normalization_required =
        decision->normalization_required ||
        std::strcmp(member.spatial_profile.group_id, target_group) != 0;
  }
  if (!peer_counted && participant_count != UINT8_MAX)
    ++participant_count;
  decision->participant_count = participant_count;
  return participant_count > MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS
             ? AutomaticSpatialGroupResult::GROUP_FULL
             : AutomaticSpatialGroupResult::OK;
}

SiteConfigurationResult apply_automatic_spatial_group_assignment(
    const PlatformIdentity &identity,
    const AutomaticSpatialGroupDecision &decision,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated, bool *changed) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(identity.site_id, current.site_id) != 0 ||
      std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0 ||
      !valid_identifier(
          decision.space_id,
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_identifier(
          decision.group_id,
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH))
    return SiteConfigurationResult::INVALID_ARGUMENT;
  *updated = current;
  bool mutated = false;
  if (decision.normalization_required) {
    for (auto &member : updated->members) {
      if (!member.occupied || !member.spatial_profile.committed ||
          std::strcmp(member.spatial_profile.space_id, decision.space_id) != 0 ||
          std::strcmp(member.spatial_profile.group_id, decision.group_id) == 0)
        continue;
      copy_text(member.spatial_profile.group_id,
                sizeof(member.spatial_profile.group_id), decision.group_id);
      mutated = true;
    }
  }
  if (!valid_configuration(*updated))
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  if (changed != nullptr)
    *changed = mutated;
  return SiteConfigurationResult::OK;
}

const char *automatic_spatial_group_result_to_string(
    AutomaticSpatialGroupResult result) {
  switch (result) {
    case AutomaticSpatialGroupResult::OK:
      return "ok";
    case AutomaticSpatialGroupResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case AutomaticSpatialGroupResult::NOT_READY:
      return "not_ready";
    case AutomaticSpatialGroupResult::GROUP_FULL:
      return "group_full";
  }
  return "invalid_argument";
}

SiteConfigurationRevisionOrder compare_site_configuration_revision(
    uint32_t left_authority_epoch, uint32_t left_generation,
    uint32_t right_authority_epoch, uint32_t right_generation) {
  if (left_authority_epoch != right_authority_epoch)
    return left_authority_epoch < right_authority_epoch
               ? SiteConfigurationRevisionOrder::OLDER
               : SiteConfigurationRevisionOrder::NEWER;
  if (left_generation != right_generation)
    return left_generation < right_generation
               ? SiteConfigurationRevisionOrder::OLDER
               : SiteConfigurationRevisionOrder::NEWER;
  return SiteConfigurationRevisionOrder::SAME;
}

SiteConfigurationRevisionOrder compare_site_configuration_revision(
    const SiteConfigurationSnapshot &left,
    const SiteConfigurationSnapshot &right) {
  return compare_site_configuration_revision(
      left.authority_epoch, left.generation, right.authority_epoch,
      right.generation);
}

namespace {

void digest_site_members(uint64_t *hash,
                         const SiteConfigurationSnapshot &configuration) {
  for (const auto &member : configuration.members) {
    digest_u8(hash, member.occupied ? 1U : 0U);
    if (!member.occupied)
      continue;
    digest_text(hash, member.participant_id);
    digest_u8(hash, static_cast<uint8_t>(member.type));
    digest_u8(hash, static_cast<uint8_t>(member.trust));
    digest_u8(hash, static_cast<uint8_t>(member.membership));
    digest_u8(hash, member.leader_eligible ? 1U : 0U);
    const auto &profile = member.spatial_profile;
    digest_u8(hash, profile.committed ? 1U : 0U);
    if (!profile.committed)
      continue;
    digest_text(hash, profile.space_id);
    digest_text(hash, profile.group_id);
    digest_u32(hash, profile.pose_version);
    digest_u32(hash, profile.calibration_version);
    digest_i32(hash, profile.transform.origin_x_mm);
    digest_i32(hash, profile.transform.origin_y_mm);
    digest_i16(hash, profile.transform.rotation_cdeg);
    digest_u32(hash, profile.transform.residual_mm);
    digest_u8(hash, profile.transform.sample_count);
    digest_u8(hash, profile.transform.valid ? 1U : 0U);
  }
}

uint64_t site_configuration_digest_with_owner_and_revision(
    const SiteConfigurationSnapshot &configuration,
    const char *configuration_owner_node_id, uint32_t roster_revision) {
  uint64_t hash = 14695981039346656037ULL;
  digest_text(&hash, configuration.site_id);
  digest_text(&hash, configuration_owner_node_id);
  digest_u32(&hash, roster_revision);
  digest_u8(&hash, configuration.roster_committed ? 1U : 0U);
  digest_u8(&hash, configuration.floorplan.committed ? 1U : 0U);
  digest_text(&hash, configuration.floorplan.floorplan_id);
  digest_u32(&hash, configuration.floorplan.geometry_version);
  digest_u32(&hash, configuration.floorplan.config_hash);
  digest_u32(&hash, configuration.floorplan.image_hash);
  digest_site_members(&hash, configuration);
  return hash == 0 ? 1 : hash;
}

uint64_t site_topology_digest_with_revision(
    const SiteConfigurationSnapshot &configuration, uint32_t roster_revision) {
  uint64_t hash = 14695981039346656037ULL;
  digest_text(&hash, configuration.site_id);
  digest_u32(&hash, roster_revision);
  digest_u8(&hash, configuration.roster_committed ? 1U : 0U);
  digest_site_members(&hash, configuration);
  return hash == 0 ? 1 : hash;
}

}  // namespace

uint64_t site_configuration_digest(
    const SiteConfigurationSnapshot &configuration) {
  return site_configuration_digest_with_owner_and_revision(
      configuration, configuration.configuration_owner_node_id,
      configuration.roster_revision);
}

uint64_t site_topology_digest(
    const SiteConfigurationSnapshot &configuration) {
  return site_topology_digest_with_revision(configuration,
                                            configuration.roster_revision);
}

uint64_t automatic_configuration_handover_fingerprint(
    const SiteConfigurationSnapshot &configuration,
    const char *successor_node_id) {
  uint64_t hash = site_configuration_digest(configuration);
  digest_u32(&hash, configuration.authority_epoch);
  digest_u32(&hash, configuration.generation);
  digest_text(&hash, successor_node_id);
  return hash == 0 ? 1 : hash;
}

bool same_site_topology(const SiteConfigurationSnapshot &left,
                        const SiteConfigurationSnapshot &right) {
  return left.roster_revision == right.roster_revision &&
         site_topology_digest(left) == site_topology_digest(right);
}

bool site_configuration_member_active(
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, bool require_leader_eligible) {
  const auto *member = configuration.find(participant_id);
  return member != nullptr &&
         member->membership == ParticipantMembership::ACTIVE &&
         (!require_leader_eligible ||
          (member->type == ParticipantType::NAMO_NODE &&
           member->leader_eligible));
}

uint8_t site_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const char *participant_id) {
  return configuration != nullptr && participant_id != nullptr &&
                 configuration->configuration_owner_node_id[0] != '\0' &&
                 std::strcmp(participant_id,
                             configuration->configuration_owner_node_id) == 0
             ? 1
             : 0;
}

uint8_t site_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, const char *participant_id) {
  const bool configuration_owner =
      site_leader_role_penalty(configuration, participant_id) != 0;
  bool current_group_leader = false;
  if (directory != nullptr && directory->committed &&
      participant_id != nullptr && participant_id[0] != '\0') {
    for (const auto &group : directory->groups) {
      if (group.occupied && group.leader_node_id[0] != '\0' &&
          std::strcmp(group.leader_node_id, participant_id) == 0) {
        current_group_leader = true;
        break;
      }
    }
  }
  if (current_group_leader)
    return configuration_owner ? 3 : 2;
  return configuration_owner ? 1 : 0;
}

uint8_t group_leader_role_penalty(
    const SiteConfigurationSnapshot *configuration,
    const char *site_leader_node_id, const char *participant_id) {
  const bool configuration_owner =
      site_leader_role_penalty(configuration, participant_id) != 0;
  const bool site_leader =
      participant_id != nullptr && site_leader_node_id != nullptr &&
      site_leader_node_id[0] != '\0' &&
      std::strcmp(participant_id, site_leader_node_id) == 0;
  if (site_leader)
    return configuration_owner ? 3 : 2;
  return configuration_owner ? 1 : 0;
}

bool group_leader_role_combination_allowed(
    const SiteConfigurationSnapshot &configuration,
    const char *site_leader_node_id, const char *participant_id) {
  if (participant_id == nullptr || participant_id[0] == '\0' ||
      site_leader_node_id == nullptr || site_leader_node_id[0] == '\0' ||
      std::strcmp(participant_id, site_leader_node_id) != 0 ||
      std::strcmp(participant_id,
                  configuration.configuration_owner_node_id) != 0)
    return true;
  const auto *candidate = configuration.find(participant_id);
  if (candidate == nullptr)
    return false;
  const char *candidate_group_id =
      candidate->spatial_profile.committed &&
              candidate->spatial_profile.group_id[0] != '\0'
          ? candidate->spatial_profile.group_id
          : nullptr;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, participant_id) == 0 ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE ||
        !member.leader_eligible || !member.spatial_profile.committed ||
        (candidate_group_id != nullptr &&
         std::strcmp(member.spatial_profile.group_id,
                     candidate_group_id) != 0))
      continue;
    return false;
  }
  return true;
}

bool group_leader_assignment_allowed(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *site_leader_node_id, const char *participant_id) {
  if (group_id == nullptr || group_id[0] == '\0' ||
      participant_id == nullptr || participant_id[0] == '\0')
    return false;
  const auto *candidate = configuration.find(participant_id);
  if (candidate == nullptr ||
      candidate->membership != ParticipantMembership::ACTIVE ||
      candidate->type != ParticipantType::NAMO_NODE ||
      !candidate->leader_eligible || !candidate->spatial_profile.committed ||
      std::strcmp(candidate->spatial_profile.group_id, group_id) != 0)
    return false;
  if (!group_leader_role_combination_allowed(
          configuration, site_leader_node_id, participant_id))
    return false;
  if (site_leader_node_id == nullptr || site_leader_node_id[0] == '\0' ||
      std::strcmp(participant_id, site_leader_node_id) != 0)
    return true;

  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE || !member.leader_eligible ||
        !member.spatial_profile.committed ||
        std::strcmp(member.participant_id, participant_id) == 0 ||
        std::strcmp(member.spatial_profile.group_id, group_id) != 0)
      continue;
    return false;
  }
  return true;
}

bool leader_candidate_preferred(
    const char *candidate_id, uint8_t candidate_penalty,
    const char *current_id, uint8_t current_penalty) {
  return current_id == nullptr || candidate_penalty < current_penalty ||
         (candidate_penalty == current_penalty &&
          std::strcmp(candidate_id, current_id) < 0);
}

bool local_node_is_enrolled_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration) {
  return configuration.roster_committed &&
         std::strcmp(configuration.site_id, identity.site_id) == 0 &&
         configuration.configuration_owner_node_id[0] != '\0' &&
         std::strcmp(configuration.configuration_owner_node_id,
                     identity.node_id) != 0 &&
         configuration.active_member_count() > 1 &&
         site_configuration_member_active(configuration, identity.node_id);
}

const SiteConfigurationMember *automatic_configuration_successor(
    const SiteConfigurationSnapshot &configuration) {
  if (!valid_configuration(configuration) ||
      !configuration.roster_committed ||
      configuration.configuration_owner_node_id[0] == '\0')
    return nullptr;

  const SiteConfigurationMember *successor = nullptr;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE ||
        !member.leader_eligible ||
        std::strcmp(member.participant_id,
                    configuration.configuration_owner_node_id) == 0)
      continue;
    if (successor == nullptr ||
        std::strcmp(member.participant_id, successor->participant_id) < 0)
      successor = &member;
  }
  return successor;
}

bool automatic_configuration_handover_bootstrap_allowed(
    const SiteConfigurationSnapshot &current,
    const char *credential_participant_id,
    const char *advertised_participant_id, uint32_t authority_epoch,
    uint32_t generation, uint64_t configuration_digest,
    uint32_t topology_revision, uint64_t topology_digest) {
  if (!valid_configuration(current) || credential_participant_id == nullptr ||
      advertised_participant_id == nullptr ||
      std::strcmp(credential_participant_id,
                  current.configuration_owner_node_id) != 0 ||
      current.generation == std::numeric_limits<uint32_t>::max())
    return false;
  const auto *advertised = current.find(advertised_participant_id);
  if (advertised == nullptr ||
      advertised->membership != ParticipantMembership::ACTIVE ||
      advertised->type != ParticipantType::NAMO_NODE ||
      !advertised->leader_eligible)
    return false;

  if (current.authority_epoch == std::numeric_limits<uint32_t>::max())
    return false;
  const uint32_t expected_authority_epoch = current.authority_epoch + 1U;
  const uint32_t expected_generation = current.generation + 1U;
  const uint32_t expected_roster_revision = current.roster_revision;
  return authority_epoch == expected_authority_epoch &&
         generation == expected_generation &&
         configuration_digest ==
             site_configuration_digest_with_owner_and_revision(
                 current, advertised->participant_id,
                 expected_roster_revision) &&
         topology_revision == expected_roster_revision &&
         topology_digest == site_topology_digest_with_revision(
                                current, expected_roster_revision);
}

bool site_configuration_needs_spatial_profile_reconciliation(
    const SiteConfigurationSnapshot &configuration) {
  if (!configuration.floorplan.committed)
    return false;
  for (const auto &member : configuration.members) {
    if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
        member.membership != ParticipantMembership::ACTIVE)
      continue;
    if (!member.spatial_profile.committed ||
        member.spatial_profile.space_id[0] == '\0' ||
        member.spatial_profile.group_id[0] == '\0')
      return true;
  }
  return false;
}

SiteConfigurationResult initialize_site_configuration(
    const PlatformIdentity &identity, bool enrolled,
    SiteConfigurationSnapshot *snapshot) {
  if (snapshot == nullptr ||
      !valid_identifier(identity.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_identifier(identity.node_id, ParticipantRecord::MAX_ID_LENGTH))
    return SiteConfigurationResult::INVALID_ARGUMENT;
  *snapshot = {};
  copy_text(snapshot->site_id, sizeof(snapshot->site_id), identity.site_id);
  snapshot->generation = 1;
  snapshot->roster_revision = 1;
  snapshot->roster_committed = !enrolled;
  auto &local = snapshot->members[0];
  local.occupied = true;
  copy_text(local.participant_id, sizeof(local.participant_id), identity.node_id);
  local.type = ParticipantType::NAMO_NODE;
  local.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  local.membership = ParticipantMembership::ACTIVE;
  local.leader_eligible = true;
  if (!enrolled)
    copy_text(snapshot->configuration_owner_node_id,
              sizeof(snapshot->configuration_owner_node_id), identity.node_id);
  return valid_configuration(*snapshot) ? SiteConfigurationResult::OK
                                        : SiteConfigurationResult::INVALID_CONFIGURATION;
}

SiteConfigurationResult reconcile_unattested_namo_trust(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated, bool *changed) {
  if (changed != nullptr)
    *changed = false;
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;

  *updated = current;
  bool mutated = false;
  for (auto &member : updated->members) {
    if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
        member.trust != ParticipantTrust::OFFICIAL_VERIFIED)
      continue;
    member.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
    mutated = true;
  }
  if (!mutated)
    return SiteConfigurationResult::OK;
  if (current.roster_revision == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  ++updated->roster_revision;
  if (!valid_configuration(*updated))
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  if (changed != nullptr)
    *changed = true;
  return SiteConfigurationResult::OK;
}

SiteConfigurationResult claim_local_site_ownership(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (current.configuration_owner_node_id[0] != '\0' ||
      current.roster_committed)
    return SiteConfigurationResult::NOT_OWNER;

  return build_local_owned_roster(identity, participants, current, updated);
}

SiteConfigurationResult commit_local_site_roster(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  return build_local_owned_roster(identity, participants, current, updated);
}

SiteConfigurationResult reconcile_local_spatial_profiles(
    const PlatformIdentity &identity,
    const ParticipantReadinessRegistry &readiness,
    uint32_t local_pose_version,
    const SpatialTransform2D &local_transform,
    const SiteConfigurationSnapshot &current,
    SpatialProfileReconciliationMode mode,
    const char *explicit_participant_id,
    SiteConfigurationSnapshot *updated, bool *changed) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0 ||
      local_pose_version == 0 || !valid_spatial_transform(local_transform) ||
      (mode == SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION &&
       !valid_identifier(explicit_participant_id,
                         ParticipantRecord::MAX_ID_LENGTH)))
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (!current.floorplan.committed)
    return SiteConfigurationResult::INVALID_CONFIGURATION;

  *updated = current;
  bool mutated = false;
  const auto *current_local = current.find(identity.node_id);
  const bool local_assignment_fixed =
      current_local != nullptr &&
      current_local->spatial_profile.committed &&
      current_local->spatial_profile.space_id[0] != '\0' &&
      current_local->spatial_profile.group_id[0] != '\0';
  uint32_t local_calibration_version = 0;
  char local_space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char local_group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  bool local_assignment_seen = false;
  bool local_assignment_ambiguous = false;
  bool explicit_profile_seen = false;
  if (local_assignment_fixed) {
    copy_text(local_space_id, sizeof(local_space_id),
              current_local->spatial_profile.space_id);
    copy_text(local_group_id, sizeof(local_group_id),
              current_local->spatial_profile.group_id);
    local_assignment_seen = true;
  }
  for (auto &member : updated->members) {
    if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
        member.membership == ParticipantMembership::REVOKED ||
        std::strcmp(member.participant_id, identity.node_id) == 0)
      continue;
    const auto *profile = readiness.find(member.participant_id);
    if (!ParticipantReadinessRegistry::ready(profile) ||
        (profile->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) == 0 ||
        std::strcmp(profile->config.floorplan_id,
                    current.floorplan.floorplan_id) != 0 ||
        profile->config.geometry_version !=
            current.floorplan.geometry_version ||
        !valid_spatial_transform(profile->config.transform))
      continue;

    SiteSpatialProfile desired{};
    desired.committed = true;
    copy_text(desired.space_id, sizeof(desired.space_id),
              profile->config.space_id);
    copy_text(desired.group_id, sizeof(desired.group_id),
              profile->config.group_id);
    desired.pose_version = profile->config.pose_version;
    desired.calibration_version = profile->config.calibration_version;
    desired.transform = profile->config.transform;
    desired.transform.residual_mm = 0;
    desired.transform.sample_count = 1;
    const bool explicit_profile =
        mode == SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION &&
        std::strcmp(member.participant_id, explicit_participant_id) == 0;
    explicit_profile_seen = explicit_profile_seen || explicit_profile;
    const bool legacy_profile_without_assignment =
        member.spatial_profile.committed &&
        member.spatial_profile.space_id[0] == '\0' &&
        member.spatial_profile.group_id[0] == '\0';
    if (legacy_profile_without_assignment) {
      if (member.spatial_profile.pose_version == desired.pose_version &&
          member.spatial_profile.calibration_version ==
              desired.calibration_version) {
        copy_text(member.spatial_profile.space_id,
                  sizeof(member.spatial_profile.space_id), desired.space_id);
        copy_text(member.spatial_profile.group_id,
                  sizeof(member.spatial_profile.group_id), desired.group_id);
        mutated = true;
      }
    } else if (!member.spatial_profile.committed) {
      member.spatial_profile = desired;
      mutated = true;
    } else if (explicit_profile &&
               !same_spatial_profile(member.spatial_profile, desired)) {
      if (desired.calibration_version <=
          member.spatial_profile.calibration_version)
        return SiteConfigurationResult::INVALID_CONFIGURATION;
      member.spatial_profile = desired;
      mutated = true;
    }

    const bool same_local_assignment =
        local_assignment_seen &&
        std::strcmp(local_space_id, profile->config.space_id) == 0 &&
        std::strcmp(local_group_id, profile->config.group_id) == 0;
    if (local_assignment_fixed) {
      if (same_local_assignment &&
          profile->config.calibration_version > local_calibration_version)
        local_calibration_version = profile->config.calibration_version;
      continue;
    }
    if (!local_assignment_seen) {
      copy_text(local_space_id, sizeof(local_space_id),
                profile->config.space_id);
      copy_text(local_group_id, sizeof(local_group_id),
                profile->config.group_id);
      local_assignment_seen = true;
    } else if (!same_local_assignment) {
      local_assignment_ambiguous = true;
    }
    if (profile->config.calibration_version > local_calibration_version)
      local_calibration_version = profile->config.calibration_version;
  }

  auto *local = static_cast<SiteConfigurationMember *>(nullptr);
  for (auto &member : updated->members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, identity.node_id) == 0) {
      local = &member;
      break;
    }
  }
  if (local == nullptr)
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  if (mode == SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION &&
      !explicit_profile_seen)
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  const bool local_assignment_ready =
      local_assignment_seen && !local_assignment_ambiguous;
  if (local_assignment_ready && local_calibration_version != 0 &&
      !local->spatial_profile.committed) {
    local->spatial_profile = {};
    local->spatial_profile.committed = true;
    local->spatial_profile.pose_version = local_pose_version;
    local->spatial_profile.calibration_version = 1;
    copy_text(local->spatial_profile.space_id,
              sizeof(local->spatial_profile.space_id), local_space_id);
    copy_text(local->spatial_profile.group_id,
              sizeof(local->spatial_profile.group_id), local_group_id);
    local->spatial_profile.transform = local_transform;
    local->spatial_profile.transform.residual_mm = 0;
    local->spatial_profile.transform.sample_count = 1;
    mutated = true;
  } else if (mode == SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION &&
             local->spatial_profile.committed &&
             (local->spatial_profile.pose_version != local_pose_version ||
              !same_transform(local->spatial_profile.transform,
                              local_transform))) {
    if (local->spatial_profile.calibration_version == UINT32_MAX)
      return SiteConfigurationResult::INVALID_CONFIGURATION;
    local->spatial_profile.pose_version = local_pose_version;
    ++local->spatial_profile.calibration_version;
    local->spatial_profile.transform = local_transform;
    local->spatial_profile.transform.residual_mm = 0;
    local->spatial_profile.transform.sample_count = 1;
    mutated = true;
  } else if (local_assignment_ready && local_calibration_version != 0 &&
             local->spatial_profile.committed &&
             local->spatial_profile.group_id[0] == '\0') {
    copy_text(local->spatial_profile.space_id,
              sizeof(local->spatial_profile.space_id), local_space_id);
    copy_text(local->spatial_profile.group_id,
              sizeof(local->spatial_profile.group_id), local_group_id);
    mutated = true;
  }
  if (!valid_configuration(*updated))
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  if (changed != nullptr)
    *changed = mutated;
  return SiteConfigurationResult::OK;
}

SiteConfigurationResult reconcile_direct_spatial_readiness(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantReadinessRegistry &current,
    ParticipantReadinessRegistry *updated, bool *changed) {
  if (updated == nullptr || !valid_configuration(configuration) ||
      std::strcmp(configuration.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;

  *updated = current;
  bool mutated = false;
  const auto *local = configuration.find(identity.node_id);
  if (!configuration.floorplan.committed || local == nullptr ||
      local->membership != ParticipantMembership::ACTIVE ||
      !local->spatial_profile.committed) {
    if (changed != nullptr)
      *changed = false;
    return SiteConfigurationResult::OK;
  }

  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant == nullptr ||
        participant->membership != ParticipantMembership::ACTIVE)
      continue;
    const auto *remote = configuration.find(participant->participant_id);
    const auto *existing = current.find(participant->participant_id);
    if (remote == nullptr || remote->membership != ParticipantMembership::ACTIVE ||
        !remote->spatial_profile.committed || existing == nullptr ||
        (existing->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) == 0)
      continue;

    ParticipantReadinessConfig desired = existing->config;
    copy_text(desired.floorplan_id, sizeof(desired.floorplan_id),
              configuration.floorplan.floorplan_id);
    copy_text(desired.space_id, sizeof(desired.space_id),
              remote->spatial_profile.space_id);
    copy_text(desired.group_id, sizeof(desired.group_id),
              remote->spatial_profile.group_id);
    desired.geometry_version = configuration.floorplan.geometry_version;
    desired.local_pose_version = local->spatial_profile.pose_version;
    desired.pose_version = remote->spatial_profile.pose_version;
    desired.calibration_version = remote->spatial_profile.calibration_version;
    desired.transform = remote->spatial_profile.transform;
    desired.transform.residual_mm = 0;
    desired.transform.sample_count = 1;
    if (!ParticipantReadinessRegistry::valid_config(desired))
      return SiteConfigurationResult::INVALID_CONFIGURATION;
    if (std::strcmp(existing->config.floorplan_id, desired.floorplan_id) == 0 &&
        std::strcmp(existing->config.space_id, desired.space_id) == 0 &&
        std::strcmp(existing->config.group_id, desired.group_id) == 0 &&
        existing->config.geometry_version == desired.geometry_version &&
        existing->config.local_pose_version == desired.local_pose_version &&
        existing->config.pose_version == desired.pose_version &&
        existing->config.calibration_version == desired.calibration_version &&
        same_transform(existing->config.transform, desired.transform))
      continue;
    if (updated->configure(participant->participant_id, desired) !=
        ParticipantReadinessResult::OK)
      return SiteConfigurationResult::INVALID_CONFIGURATION;
    mutated = true;
  }
  if (changed != nullptr)
    *changed = mutated;
  return SiteConfigurationResult::OK;
}

SiteConfigurationResult prepare_local_pairing_site(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current, const char *peer_participant_id,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || peer_participant_id == nullptr ||
      !valid_identifier(peer_participant_id,
                        ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;

  const bool owner_unresolved =
      current.configuration_owner_node_id[0] == '\0';
  const bool local_owner =
      std::strcmp(current.configuration_owner_node_id, identity.node_id) == 0;
  if (owner_unresolved)
    return claim_local_site_ownership(identity, participants, current, updated);
  if (local_owner)
    return commit_local_site_roster(identity, participants, current, updated);
  if (std::strcmp(current.configuration_owner_node_id,
                  peer_participant_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;

  for (const auto &member : current.members) {
    if (!member.occupied ||
        member.membership == ParticipantMembership::REVOKED ||
        std::strcmp(member.participant_id, identity.node_id) == 0 ||
        std::strcmp(member.participant_id, peer_participant_id) == 0)
      continue;
    return SiteConfigurationResult::NOT_OWNER;
  }
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant == nullptr ||
        participant->membership == ParticipantMembership::REVOKED ||
        std::strcmp(participant->participant_id, peer_participant_id) == 0)
      continue;
    return SiteConfigurationResult::NOT_OWNER;
  }

  return build_local_owned_roster(identity, participants, current, updated);
}

bool pairing_joiner_site_already_converged(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *leader_site_id, const char *owner_node_id) {
  if (leader_site_id == nullptr || owner_node_id == nullptr ||
      !valid_identifier(leader_site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_identifier(owner_node_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_configuration(configuration) || !configuration.roster_committed ||
      std::strcmp(identity.site_id, leader_site_id) != 0 ||
      std::strcmp(configuration.site_id, leader_site_id) != 0 ||
      std::strcmp(configuration.configuration_owner_node_id,
                  owner_node_id) != 0)
    return false;
  const auto *local = configuration.find(identity.node_id);
  const auto *owner = configuration.find(owner_node_id);
  return local != nullptr &&
         local->type == ParticipantType::NAMO_NODE &&
         local->membership != ParticipantMembership::REVOKED &&
         owner != nullptr && owner->type == ParticipantType::NAMO_NODE &&
         owner->membership == ParticipantMembership::ACTIVE &&
         owner->leader_eligible;
}

bool is_local_standalone_site_configuration(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current) {
  if (!valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0 ||
      std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0 ||
      !current.roster_committed || current.member_count() != 1 ||
      current.active_member_count() != 1)
    return false;
  const auto *local = current.find(identity.node_id);
  if (local == nullptr || local->type != ParticipantType::NAMO_NODE ||
      local->trust != ParticipantTrust::COMMUNITY_UNVERIFIED ||
      local->membership != ParticipantMembership::ACTIVE ||
      !local->leader_eligible)
    return false;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant != nullptr &&
        participant->membership != ParticipantMembership::REVOKED)
      return false;
  }
  return true;
}

SiteConfigurationResult reconcile_local_standalone_site_configuration(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current))
    return SiteConfigurationResult::INVALID_ARGUMENT;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant != nullptr &&
        participant->membership != ParticipantMembership::REVOKED)
      return SiteConfigurationResult::INVALID_CONFIGURATION;
  }

  if (std::strcmp(current.site_id, identity.site_id) != 0)
    return initialize_site_configuration(identity, false, updated);

  return build_local_owned_roster(identity, participants, current, updated);
}

const char *standalone_site_reconciliation_state_to_string(
    StandaloneSiteReconciliationState state) {
  switch (state) {
    case StandaloneSiteReconciliationState::NOT_REQUIRED:
      return "not_required";
    case StandaloneSiteReconciliationState::PENDING:
      return "pending";
    case StandaloneSiteReconciliationState::COMPLETE:
      return "complete";
    case StandaloneSiteReconciliationState::FAILED:
      return "failed";
  }
  return "unknown";
}

const char *standalone_site_reconciliation_result_to_string(
    StandaloneSiteReconciliationResult result) {
  switch (result) {
    case StandaloneSiteReconciliationResult::IDLE:
      return "idle";
    case StandaloneSiteReconciliationResult::OK:
      return "ok";
    case StandaloneSiteReconciliationResult::INVALID_STATE:
      return "invalid_state";
    case StandaloneSiteReconciliationResult::BUILD_FAILED:
      return "build_failed";
    case StandaloneSiteReconciliationResult::SITE_STORAGE_FAILED:
      return "site_storage_failed";
    case StandaloneSiteReconciliationResult::MEMBERSHIP_STORAGE_FAILED:
      return "membership_storage_failed";
  }
  return "unknown";
}

void StandaloneSiteReconciliationTracker::mark_not_required() {
  this->state_ = StandaloneSiteReconciliationState::NOT_REQUIRED;
  this->result_ = StandaloneSiteReconciliationResult::IDLE;
  this->due_ms_ = 0;
}

void StandaloneSiteReconciliationTracker::schedule(uint32_t now_ms,
                                                   uint32_t delay_ms) {
  if (this->state_ == StandaloneSiteReconciliationState::PENDING)
    return;
  this->state_ = StandaloneSiteReconciliationState::PENDING;
  this->result_ = StandaloneSiteReconciliationResult::IDLE;
  this->due_ms_ = now_ms + delay_ms;
}

bool StandaloneSiteReconciliationTracker::due(uint32_t now_ms) const {
  return this->state_ == StandaloneSiteReconciliationState::PENDING &&
         static_cast<int32_t>(now_ms - this->due_ms_) >= 0;
}

void StandaloneSiteReconciliationTracker::complete(
    StandaloneSiteReconciliationResult result) {
  this->result_ = result;
  this->state_ = result == StandaloneSiteReconciliationResult::OK
                     ? StandaloneSiteReconciliationState::COMPLETE
                     : StandaloneSiteReconciliationState::FAILED;
  this->due_ms_ = 0;
}

SiteConfigurationResult commit_local_floorplan_manifest(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *floorplan_id,
    uint32_t geometry_version, uint32_t config_hash, uint32_t image_hash,
    SiteConfigurationSnapshot *updated, int32_t coordinate_width_mm,
    int32_t coordinate_height_mm) {
  if (updated == nullptr || floorplan_id == nullptr ||
      !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0 ||
      !valid_identifier(floorplan_id, ParticipantRecord::MAX_ID_LENGTH) ||
      geometry_version == 0 || config_hash == 0 || image_hash == 0 ||
      coordinate_width_mm <= 0 || coordinate_height_mm <= 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;

  *updated = current;
  const bool geometry_changed =
      !current.floorplan.committed ||
      std::strcmp(current.floorplan.floorplan_id, floorplan_id) != 0 ||
      current.floorplan.geometry_version != geometry_version;
  if (geometry_changed) {
    const int32_t center_x_mm = coordinate_width_mm / 2;
    const int32_t center_y_mm = coordinate_height_mm / 2;
    for (auto &member : updated->members) {
      auto &profile = member.spatial_profile;
      if (!member.occupied || !profile.committed ||
          !profile.transform.valid)
        continue;
      const bool outside = profile.transform.origin_x_mm < 0 ||
                           profile.transform.origin_y_mm < 0 ||
                           profile.transform.origin_x_mm > coordinate_width_mm ||
                           profile.transform.origin_y_mm > coordinate_height_mm;
      if (!outside)
        continue;
      if (profile.calibration_version == UINT32_MAX)
        return SiteConfigurationResult::INVALID_CONFIGURATION;
      profile.transform.origin_x_mm = center_x_mm;
      profile.transform.origin_y_mm = center_y_mm;
      profile.transform.residual_mm = 0;
      profile.transform.sample_count = 1;
      ++profile.calibration_version;
    }
  }
  updated->floorplan = {};
  updated->floorplan.committed = true;
  copy_text(updated->floorplan.floorplan_id,
            sizeof(updated->floorplan.floorplan_id), floorplan_id);
  updated->floorplan.geometry_version = geometry_version;
  updated->floorplan.config_hash = config_hash;
  updated->floorplan.image_hash = image_hash;
  return valid_configuration(*updated) ? SiteConfigurationResult::OK
                                        : SiteConfigurationResult::INVALID_CONFIGURATION;
}

bool site_configuration_has_remote_members(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration) {
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership != ParticipantMembership::REVOKED &&
        std::strcmp(member.participant_id, identity.node_id) != 0)
      return true;
  }
  return false;
}

bool site_floorplan_manifest_matches(
    const SiteConfigurationSnapshot &current, const char *floorplan_id,
    uint32_t geometry_version, uint32_t config_hash, uint32_t image_hash) {
  return floorplan_id != nullptr && current.floorplan.committed &&
         std::strcmp(current.floorplan.floorplan_id, floorplan_id) == 0 &&
         current.floorplan.geometry_version == geometry_version &&
         current.floorplan.config_hash == config_hash &&
         current.floorplan.image_hash == image_hash;
}

SiteConfigurationResult clear_local_floorplan_manifest(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;

  *updated = current;
  clear_spatial_profiles(updated);
  updated->floorplan = {};
  return valid_configuration(*updated) ? SiteConfigurationResult::OK
                                        : SiteConfigurationResult::INVALID_CONFIGURATION;
}

SiteConfigurationResult prepare_automatic_configuration_handover(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated,
    const char *designated_successor_node_id) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  const auto *successor = designated_successor_node_id != nullptr
                              ? current.find(designated_successor_node_id)
                              : automatic_configuration_successor(current);
  if (successor == nullptr ||
      successor->membership != ParticipantMembership::ACTIVE ||
      successor->type != ParticipantType::NAMO_NODE ||
      !successor->leader_eligible ||
      std::strcmp(successor->participant_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (current.authority_epoch == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;

  *updated = current;
  copy_text(updated->configuration_owner_node_id,
            sizeof(updated->configuration_owner_node_id), identity.node_id);
  // Ownership authority changes without changing canonical membership.
  ++updated->authority_epoch;
  return valid_configuration(*updated)
             ? SiteConfigurationResult::OK
             : SiteConfigurationResult::INVALID_CONFIGURATION;
}

SiteConfigurationResult prepare_local_configuration_conflict_resolution(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (current.active_member_count() <= 1 ||
      current.authority_epoch == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  *updated = current;
  // Selecting an existing payload does not publish a new Site topology.
  ++updated->authority_epoch;
  return valid_configuration(*updated)
             ? SiteConfigurationResult::OK
             : SiteConfigurationResult::INVALID_CONFIGURATION;
}

SiteConfigurationResult set_local_site_member_leader_eligible(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    bool leader_eligible, SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || participant_id == nullptr ||
      participant_id[0] == '\0' || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (std::strcmp(participant_id, identity.node_id) == 0)
    return SiteConfigurationResult::INVALID_CONFIGURATION;

  const auto *existing = current.find(participant_id);
  if (existing == nullptr)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (existing->type != ParticipantType::NAMO_NODE ||
      existing->trust != ParticipantTrust::COMMUNITY_UNVERIFIED ||
      existing->membership != ParticipantMembership::ACTIVE)
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  if (existing->leader_eligible == leader_eligible) {
    *updated = current;
    return SiteConfigurationResult::OK;
  }
  if (current.roster_revision == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;

  *updated = current;
  for (auto &member : updated->members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, participant_id) != 0)
      continue;
    member.leader_eligible = leader_eligible;
    ++updated->roster_revision;
    return valid_configuration(*updated)
               ? SiteConfigurationResult::OK
               : SiteConfigurationResult::INVALID_CONFIGURATION;
  }
  return SiteConfigurationResult::INVALID_CONFIGURATION;
}

SiteConfigurationResult revoke_local_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || participant_id == nullptr ||
      participant_id[0] == '\0' || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (std::strcmp(participant_id, identity.node_id) == 0 ||
      current.roster_revision == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  const auto *existing = current.find(participant_id);
  if (existing == nullptr)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (existing->membership == ParticipantMembership::REVOKED) {
    *updated = current;
    return SiteConfigurationResult::OK;
  }

  *updated = current;
  for (auto &member : updated->members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, participant_id) != 0)
      continue;
    member.membership = ParticipantMembership::REVOKED;
    member.leader_eligible = false;
    member.spatial_profile = {};
    ++updated->roster_revision;
    return valid_configuration(*updated)
               ? SiteConfigurationResult::OK
               : SiteConfigurationResult::INVALID_CONFIGURATION;
  }
  return SiteConfigurationResult::INVALID_ARGUMENT;
}

SiteConfigurationResult forget_local_site_member(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current, const char *participant_id,
    SiteConfigurationSnapshot *updated) {
  if (updated == nullptr || participant_id == nullptr ||
      participant_id[0] == '\0' || !valid_configuration(current) ||
      std::strcmp(current.site_id, identity.site_id) != 0)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (std::strcmp(current.configuration_owner_node_id, identity.node_id) != 0)
    return SiteConfigurationResult::NOT_OWNER;
  if (std::strcmp(participant_id, identity.node_id) == 0 ||
      current.roster_revision == std::numeric_limits<uint32_t>::max())
    return SiteConfigurationResult::INVALID_CONFIGURATION;
  const auto *existing = current.find(participant_id);
  if (existing == nullptr)
    return SiteConfigurationResult::INVALID_ARGUMENT;
  if (existing->membership != ParticipantMembership::REVOKED)
    return SiteConfigurationResult::INVALID_CONFIGURATION;

  *updated = current;
  for (auto &member : updated->members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, participant_id) != 0)
      continue;
    member = {};
    sort_members(updated);
    ++updated->roster_revision;
    return valid_configuration(*updated)
               ? SiteConfigurationResult::OK
               : SiteConfigurationResult::INVALID_CONFIGURATION;
  }
  return SiteConfigurationResult::INVALID_ARGUMENT;
}

bool can_move_site_without_orphaning_participants(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const char *retry_participant_id) {
  if (!valid_configuration(configuration) ||
      configuration.active_member_count() > 1)
    return false;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant == nullptr ||
        participant->membership == ParticipantMembership::REVOKED ||
        (retry_participant_id != nullptr &&
         std::strcmp(participant->participant_id, retry_participant_id) == 0))
      continue;
    return false;
  }
  return true;
}

SiteConfigurationReplicationResult validate_replicated_site_configuration(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const SiteConfigurationSnapshot &remote,
    SiteConfigurationReplicationMode mode,
    const char *source_node_id) {
  if (!valid_configuration(current) || !valid_configuration(remote) ||
      !valid_identifier(identity.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_identifier(identity.node_id, ParticipantRecord::MAX_ID_LENGTH))
    return SiteConfigurationReplicationResult::INVALID_ARGUMENT;
  if (std::strcmp(remote.site_id, identity.site_id) != 0)
    return SiteConfigurationReplicationResult::SITE_MISMATCH;

  const bool authenticated_enrollment =
      mode == SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT;
  const bool quorum_authority_rejoin =
      mode == SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN;
  if (authenticated_enrollment) {
    const auto *local = current.find(identity.node_id);
    const auto *remote_local = remote.find(identity.node_id);
    const auto *remote_owner =
        source_node_id != nullptr ? remote.find(source_node_id) : nullptr;
    if (source_node_id == nullptr || source_node_id[0] == '\0' ||
        current.configuration_owner_node_id[0] != '\0' ||
        current.roster_committed || current.member_count() != 1 ||
        current.active_member_count() != 1 || local == nullptr ||
        local->type != ParticipantType::NAMO_NODE ||
        local->membership != ParticipantMembership::ACTIVE ||
        !local->leader_eligible || !remote.roster_committed ||
        std::strcmp(remote.configuration_owner_node_id, source_node_id) != 0 ||
        remote_owner == nullptr ||
        remote_owner->type != ParticipantType::NAMO_NODE ||
        remote_owner->membership != ParticipantMembership::ACTIVE ||
        !remote_owner->leader_eligible || remote_local == nullptr ||
        remote_local->type != ParticipantType::NAMO_NODE ||
        remote_local->membership == ParticipantMembership::REVOKED)
      return SiteConfigurationReplicationResult::OWNER_MISMATCH;
  }
  if (quorum_authority_rejoin) {
    const auto *current_source =
        source_node_id != nullptr ? current.find(source_node_id) : nullptr;
    const auto *remote_source =
        source_node_id != nullptr ? remote.find(source_node_id) : nullptr;
    const auto *remote_owner =
        remote.find(remote.configuration_owner_node_id);
    if (source_node_id == nullptr || source_node_id[0] == '\0' ||
        current_source == nullptr || remote_source == nullptr ||
        current_source->membership != ParticipantMembership::ACTIVE ||
        current_source->type != ParticipantType::NAMO_NODE ||
        remote_source->membership != ParticipantMembership::ACTIVE ||
        remote_source->type != ParticipantType::NAMO_NODE ||
        remote_owner == nullptr ||
        remote_owner->membership != ParticipantMembership::ACTIVE ||
        remote_owner->type != ParticipantType::NAMO_NODE ||
        !remote_owner->leader_eligible)
      return SiteConfigurationReplicationResult::OWNER_MISMATCH;
  }
  const bool owner_changed =
      current.configuration_owner_node_id[0] != '\0' &&
      std::strcmp(remote.configuration_owner_node_id,
                  current.configuration_owner_node_id) != 0;
  if (owner_changed && !quorum_authority_rejoin) {
    const auto *successor = current.find(remote.configuration_owner_node_id);
    if (mode != SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER ||
        (source_node_id != nullptr && source_node_id[0] != '\0' &&
         std::strcmp(remote.configuration_owner_node_id, source_node_id) != 0) ||
        successor == nullptr ||
        successor->membership != ParticipantMembership::ACTIVE ||
        successor->type != ParticipantType::NAMO_NODE ||
        !successor->leader_eligible ||
        current.authority_epoch == std::numeric_limits<uint32_t>::max() ||
        remote.authority_epoch != current.authority_epoch + 1U ||
        std::strcmp(remote.configuration_owner_node_id,
                    successor->participant_id) != 0)
      return SiteConfigurationReplicationResult::OWNER_MISMATCH;
  } else if (!quorum_authority_rejoin &&
             remote.authority_epoch != current.authority_epoch) {
    if (!authenticated_enrollment &&
        (current.configuration_owner_node_id[0] == '\0' ||
        current.authority_epoch == std::numeric_limits<uint32_t>::max() ||
        remote.authority_epoch != current.authority_epoch + 1U))
      return SiteConfigurationReplicationResult::OWNER_MISMATCH;
  }
  if (remote.configuration_owner_node_id[0] == '\0' ||
      !member_can_receive_site_replica(remote, identity.node_id))
    return SiteConfigurationReplicationResult::LOCAL_MEMBER_MISSING;
  const auto revision = compare_site_configuration_revision(remote, current);
  if (revision == SiteConfigurationRevisionOrder::OLDER)
    return SiteConfigurationReplicationResult::STALE_REMOTE;
  if (revision == SiteConfigurationRevisionOrder::SAME) {
    if (same_configuration(current, remote))
      return SiteConfigurationReplicationResult::ALREADY_CURRENT;
    if (!quorum_authority_rejoin || current.active_member_count() <= 2 ||
        !same_site_topology(current, remote))
      return SiteConfigurationReplicationResult::REVISION_CONFLICT;
    auto expected = current;
    copy_text(expected.configuration_owner_node_id,
              sizeof(expected.configuration_owner_node_id),
              remote.configuration_owner_node_id);
    return same_configuration(expected, remote)
               ? SiteConfigurationReplicationResult::OK
               : SiteConfigurationReplicationResult::REVISION_CONFLICT;
  }
  return SiteConfigurationReplicationResult::OK;
}

bool authenticated_enrollment_source_allowed(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials,
    const char *source_node_id) {
  if (source_node_id == nullptr || source_node_id[0] == '\0' ||
      std::strcmp(source_node_id, identity.node_id) == 0 ||
      std::strcmp(current.site_id, identity.site_id) != 0 ||
      current.configuration_owner_node_id[0] != '\0' ||
      current.roster_committed || current.member_count() != 1 ||
      current.active_member_count() != 1)
    return false;

  const auto *local = current.find(identity.node_id);
  const auto *participant = participants.find(source_node_id);
  const auto *credential = credentials.find(source_node_id);
  if (local == nullptr || local->type != ParticipantType::NAMO_NODE ||
      local->membership != ParticipantMembership::ACTIVE ||
      !local->leader_eligible || participant == nullptr ||
      participant->type != ParticipantType::NAMO_NODE ||
      participant->trust != ParticipantTrust::COMMUNITY_UNVERIFIED ||
      participant->membership != ParticipantMembership::QUARANTINED ||
      credential == nullptr || !credential->enabled)
    return false;

  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *other = participants.record_at(slot);
    if (other != nullptr &&
        other->membership != ParticipantMembership::REVOKED &&
        std::strcmp(other->participant_id, source_node_id) != 0)
      return false;
    const auto *other_credential = credentials.record_at(slot);
    if (other_credential != nullptr &&
        std::strcmp(other_credential->participant_id, source_node_id) != 0) {
      const auto *revoked = participants.find(
          other_credential->participant_id);
      if (revoked == nullptr ||
          revoked->membership != ParticipantMembership::REVOKED ||
          other_credential->enabled)
        return false;
    }
  }
  return true;
}

const char *automatic_configuration_handover_condition_to_string(
    AutomaticConfigurationHandoverCondition condition) {
  switch (condition) {
    case AutomaticConfigurationHandoverCondition::NOT_REQUIRED:
      return "not_required";
    case AutomaticConfigurationHandoverCondition::OWNER_ONLINE:
      return "owner_online";
    case AutomaticConfigurationHandoverCondition::NO_SUCCESSOR:
      return "no_successor";
    case AutomaticConfigurationHandoverCondition::NOT_DESIGNATED_SUCCESSOR:
      return "not_designated_successor";
    case AutomaticConfigurationHandoverCondition::CONFIGURATION_CONFLICT:
      return "configuration_conflict";
    case AutomaticConfigurationHandoverCondition::QUORUM_UNAVAILABLE:
      return "quorum_unavailable";
    case AutomaticConfigurationHandoverCondition::REPLICA_NOT_READY:
      return "replica_not_ready";
    case AutomaticConfigurationHandoverCondition::CREDENTIALS_INCOMPLETE:
      return "credentials_incomplete";
    case AutomaticConfigurationHandoverCondition::ELIGIBLE:
      return "eligible";
  }
  return "not_required";
}

const char *automatic_configuration_handover_state_to_string(
    AutomaticConfigurationHandoverState state) {
  switch (state) {
    case AutomaticConfigurationHandoverState::IDLE: return "idle";
    case AutomaticConfigurationHandoverState::BLOCKED: return "blocked";
    case AutomaticConfigurationHandoverState::WAITING: return "waiting";
    case AutomaticConfigurationHandoverState::READY: return "ready";
    case AutomaticConfigurationHandoverState::COMPLETE: return "complete";
    case AutomaticConfigurationHandoverState::FAILED: return "failed";
  }
  return "idle";
}

AutomaticConfigurationHandoverCondition
evaluate_automatic_configuration_handover(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration, bool owner_online,
    bool quorum_available, bool local_replica_ready,
    bool credentials_complete, const char *designated_successor_node_id) {
  if (!valid_configuration(configuration) ||
      !configuration.roster_committed ||
      configuration.active_member_count() <= 1 ||
      std::strcmp(configuration.site_id, identity.site_id) != 0 ||
      std::strcmp(configuration.configuration_owner_node_id,
                  identity.node_id) == 0)
    return AutomaticConfigurationHandoverCondition::NOT_REQUIRED;
  if (owner_online)
    return AutomaticConfigurationHandoverCondition::OWNER_ONLINE;
  const auto *successor = designated_successor_node_id != nullptr
                              ? configuration.find(designated_successor_node_id)
                              : automatic_configuration_successor(configuration);
  if (successor == nullptr)
    return AutomaticConfigurationHandoverCondition::NO_SUCCESSOR;
  if (successor->membership != ParticipantMembership::ACTIVE ||
      successor->type != ParticipantType::NAMO_NODE ||
      !successor->leader_eligible)
    return AutomaticConfigurationHandoverCondition::NO_SUCCESSOR;
  if (std::strcmp(successor->participant_id, identity.node_id) != 0)
    return AutomaticConfigurationHandoverCondition::NOT_DESIGNATED_SUCCESSOR;
  if (!quorum_available)
    return AutomaticConfigurationHandoverCondition::QUORUM_UNAVAILABLE;
  if (configuration.floorplan.committed && !local_replica_ready)
    return AutomaticConfigurationHandoverCondition::REPLICA_NOT_READY;
  if (!credentials_complete)
    return AutomaticConfigurationHandoverCondition::CREDENTIALS_INCOMPLETE;
  return AutomaticConfigurationHandoverCondition::ELIGIBLE;
}

bool AutomaticConfigurationHandoverTracker::update(
    AutomaticConfigurationHandoverCondition condition,
    uint64_t authority_fingerprint, uint32_t now_ms) {
  const bool candidate_changed =
      condition == AutomaticConfigurationHandoverCondition::ELIGIBLE &&
      authority_fingerprint != this->authority_fingerprint_;
  if (condition != this->condition_ || candidate_changed) {
    this->condition_ = condition;
    this->stable_since_ms_ = 0;
    this->retry_due_ms_ = 0;
    this->retry_count_ = 0;
    this->authority_fingerprint_ =
        condition == AutomaticConfigurationHandoverCondition::ELIGIBLE
            ? authority_fingerprint
            : 0;
    this->state_ =
        condition == AutomaticConfigurationHandoverCondition::NOT_REQUIRED ||
                condition ==
                    AutomaticConfigurationHandoverCondition::OWNER_ONLINE ||
                condition ==
                    AutomaticConfigurationHandoverCondition::
                        NOT_DESIGNATED_SUCCESSOR
            ? AutomaticConfigurationHandoverState::IDLE
            : condition == AutomaticConfigurationHandoverCondition::ELIGIBLE
                  ? AutomaticConfigurationHandoverState::WAITING
                  : AutomaticConfigurationHandoverState::BLOCKED;
  }
  if (condition != AutomaticConfigurationHandoverCondition::ELIGIBLE)
    return false;
  if (this->state_ == AutomaticConfigurationHandoverState::COMPLETE)
    return false;
  if (this->state_ == AutomaticConfigurationHandoverState::FAILED) {
    if (static_cast<int32_t>(now_ms - this->retry_due_ms_) < 0)
      return false;
    this->state_ = AutomaticConfigurationHandoverState::READY;
    return true;
  }
  if (this->stable_since_ms_ == 0) {
    this->stable_since_ms_ = now_ms == 0 ? 1 : now_ms;
    this->state_ = AutomaticConfigurationHandoverState::WAITING;
    return false;
  }
  if (static_cast<uint32_t>(now_ms - this->stable_since_ms_) < STABILITY_MS)
    return false;
  this->state_ = AutomaticConfigurationHandoverState::READY;
  return true;
}

void AutomaticConfigurationHandoverTracker::complete(bool success,
                                                      uint32_t now_ms) {
  if (success) {
    this->state_ = AutomaticConfigurationHandoverState::COMPLETE;
    this->retry_due_ms_ = 0;
    this->retry_count_ = 0;
    return;
  }
  this->state_ = AutomaticConfigurationHandoverState::FAILED;
  if (this->retry_count_ < 5)
    ++this->retry_count_;
  const uint8_t exponent = this->retry_count_ > 4
                               ? 4
                               : static_cast<uint8_t>(this->retry_count_ - 1);
  const uint32_t base = RETRY_BASE_MS << exponent;
  const uint32_t jitter = static_cast<uint32_t>(
      (this->authority_fingerprint_ ^
       (static_cast<uint64_t>(this->retry_count_) *
        0x9E3779B97F4A7C15ULL)) %
      1001U);
  const uint32_t delay =
      base >= RETRY_MAX_MS || jitter >= RETRY_MAX_MS - base
          ? RETRY_MAX_MS
          : base + jitter;
  this->retry_due_ms_ = now_ms + delay;
}

void AutomaticConfigurationHandoverTracker::reset() {
  *this = {};
}

uint32_t AutomaticConfigurationHandoverTracker::wait_remaining_ms(
    uint32_t now_ms) const {
  if (this->state_ == AutomaticConfigurationHandoverState::FAILED) {
    if (static_cast<int32_t>(now_ms - this->retry_due_ms_) >= 0)
      return 0;
    return this->retry_due_ms_ - now_ms;
  }
  if (this->state_ == AutomaticConfigurationHandoverState::WAITING &&
      this->stable_since_ms_ != 0) {
    const uint32_t elapsed = now_ms - this->stable_since_ms_;
    return elapsed >= STABILITY_MS ? 0 : STABILITY_MS - elapsed;
  }
  return 0;
}

const char *site_configuration_replication_result_to_string(
    SiteConfigurationReplicationResult result) {
  switch (result) {
    case SiteConfigurationReplicationResult::NOT_EVALUATED: return "not_evaluated";
    case SiteConfigurationReplicationResult::OK: return "ok";
    case SiteConfigurationReplicationResult::ALREADY_CURRENT: return "already_current";
    case SiteConfigurationReplicationResult::INVALID_ARGUMENT: return "invalid_argument";
    case SiteConfigurationReplicationResult::SITE_MISMATCH: return "site_mismatch";
    case SiteConfigurationReplicationResult::OWNER_MISMATCH: return "owner_mismatch";
    case SiteConfigurationReplicationResult::LOCAL_MEMBER_MISSING: return "local_member_missing";
    case SiteConfigurationReplicationResult::STALE_REMOTE: return "stale_remote";
    case SiteConfigurationReplicationResult::REVISION_CONFLICT: return "revision_conflict";
  }
  return "invalid_argument";
}

const char *site_configuration_replication_mode_to_string(
    SiteConfigurationReplicationMode mode) {
  switch (mode) {
    case SiteConfigurationReplicationMode::STRICT: return "strict";
    case SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER:
      return "automatic_owner_handover";
    case SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT:
      return "authenticated_enrollment";
    case SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN:
      return "quorum_authority_rejoin";
  }
  return "strict";
}

SiteActivationAdmissionResult validate_namo_node_activation(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantReadinessRecord *readiness, const char *participant_id,
    bool endpoint_online, bool replica_ready,
    uint32_t remote_configuration_generation,
    uint32_t remote_geometry_version, uint32_t remote_config_hash,
    uint32_t remote_image_hash) {
  if (!valid_configuration(configuration) || participant_id == nullptr ||
      participant_id[0] == '\0')
    return SiteActivationAdmissionResult::INVALID_ARGUMENT;
  if (!configuration.roster_committed)
    return SiteActivationAdmissionResult::ROSTER_NOT_COMMITTED;
  if (!configuration.floorplan.committed)
    return SiteActivationAdmissionResult::FLOORPLAN_NOT_COMMITTED;
  const auto *member = configuration.find(participant_id);
  if (member == nullptr || member->type != ParticipantType::NAMO_NODE ||
      member->membership == ParticipantMembership::REVOKED)
    return SiteActivationAdmissionResult::MEMBER_MISSING;
  if (!ParticipantReadinessRegistry::ready(readiness))
    return SiteActivationAdmissionResult::READINESS_INCOMPLETE;
  if (std::strcmp(readiness->config.floorplan_id,
                  configuration.floorplan.floorplan_id) != 0)
    return SiteActivationAdmissionResult::FLOORPLAN_MISMATCH;
  std::size_t active_group_members = 0;
  for (const auto &candidate : configuration.members) {
    if (!candidate.occupied ||
        candidate.membership != ParticipantMembership::ACTIVE ||
        !candidate.spatial_profile.committed ||
        std::strcmp(candidate.participant_id, participant_id) == 0 ||
        std::strcmp(candidate.spatial_profile.group_id,
                    readiness->config.group_id) != 0)
      continue;
    ++active_group_members;
  }
  if (active_group_members >= GroupMembershipView::MAX_PARTICIPANTS)
    return SiteActivationAdmissionResult::GROUP_FULL;
  if (!endpoint_online)
    return SiteActivationAdmissionResult::ENDPOINT_OFFLINE;
  if (!replica_ready)
    return SiteActivationAdmissionResult::REPLICA_NOT_READY;
  if (remote_configuration_generation != configuration.generation ||
      remote_geometry_version != configuration.floorplan.geometry_version ||
      remote_config_hash != configuration.floorplan.config_hash ||
      remote_image_hash != configuration.floorplan.image_hash)
    return SiteActivationAdmissionResult::REVISION_MISMATCH;
  return SiteActivationAdmissionResult::OK;
}

const char *site_activation_admission_result_to_string(
    SiteActivationAdmissionResult result) {
  switch (result) {
    case SiteActivationAdmissionResult::OK: return "ok";
    case SiteActivationAdmissionResult::INVALID_ARGUMENT: return "invalid_argument";
    case SiteActivationAdmissionResult::ROSTER_NOT_COMMITTED: return "roster_not_committed";
    case SiteActivationAdmissionResult::FLOORPLAN_NOT_COMMITTED: return "floorplan_not_committed";
    case SiteActivationAdmissionResult::MEMBER_MISSING: return "member_missing";
    case SiteActivationAdmissionResult::READINESS_INCOMPLETE: return "readiness_incomplete";
    case SiteActivationAdmissionResult::FLOORPLAN_MISMATCH: return "floorplan_mismatch";
    case SiteActivationAdmissionResult::ENDPOINT_OFFLINE: return "endpoint_offline";
    case SiteActivationAdmissionResult::REPLICA_NOT_READY: return "replica_not_ready";
    case SiteActivationAdmissionResult::REVISION_MISMATCH: return "revision_mismatch";
    case SiteActivationAdmissionResult::GROUP_FULL: return "group_full";
  }
  return "invalid_argument";
}

SiteConfigurationCodecResult SiteConfigurationCodec::encode(
    const SiteConfigurationSnapshot &snapshot, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return SiteConfigurationCodecResult::INVALID_ARGUMENT;
  if (!valid_configuration(snapshot))
    return SiteConfigurationCodecResult::INVALID_CONFIGURATION;

  const std::size_t site_length = std::strlen(snapshot.site_id);
  const std::size_t owner_length =
      std::strlen(snapshot.configuration_owner_node_id);
  const std::size_t floorplan_length =
      std::strlen(snapshot.floorplan.floorplan_id);
  std::size_t encoded_size = HEADER_SIZE + site_length + owner_length +
                             floorplan_length + CHECKSUM_SIZE;
  for (const auto &member : snapshot.members) {
    if (member.occupied) {
      encoded_size += RECORD_HEADER_SIZE + std::strlen(member.participant_id);
      if (member.spatial_profile.committed) {
        encoded_size +=
            SPATIAL_PROFILE_V3_HEADER_SIZE + SPATIAL_PROFILE_V2_SIZE +
            std::strlen(member.spatial_profile.space_id) +
            std::strlen(member.spatial_profile.group_id);
      }
    }
  }
  if (encoded_size > output_capacity)
    return SiteConfigurationCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, CONFIG_MAGIC, sizeof(CONFIG_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(snapshot.member_count());
  output[7] = (snapshot.roster_committed ? FLAG_ROSTER_COMMITTED : 0U) |
              (snapshot.floorplan.committed ? FLAG_FLOORPLAN_COMMITTED : 0U);
  write_u32(output + 8, snapshot.generation);
  write_u32(output + 12, snapshot.roster_revision);
  output[16] = static_cast<uint8_t>(site_length);
  output[17] = static_cast<uint8_t>(owner_length);
  output[18] = static_cast<uint8_t>(floorplan_length);
  output[19] = 0;
  write_u32(output + 20, snapshot.floorplan.geometry_version);
  write_u32(output + 24, snapshot.floorplan.config_hash);
  write_u32(output + 28, snapshot.floorplan.image_hash);
  write_u32(output + 32, snapshot.authority_epoch);

  std::size_t offset = HEADER_SIZE;
  std::memcpy(output + offset, snapshot.site_id, site_length);
  offset += site_length;
  std::memcpy(output + offset, snapshot.configuration_owner_node_id,
              owner_length);
  offset += owner_length;
  std::memcpy(output + offset, snapshot.floorplan.floorplan_id,
              floorplan_length);
  offset += floorplan_length;
  for (const auto &member : snapshot.members) {
    if (!member.occupied)
      continue;
    const std::size_t id_length = std::strlen(member.participant_id);
    output[offset++] = static_cast<uint8_t>(id_length);
    output[offset++] = static_cast<uint8_t>(member.type);
    output[offset++] = static_cast<uint8_t>(member.trust);
    output[offset++] = static_cast<uint8_t>(member.membership);
    output[offset++] =
        (member.leader_eligible ? MEMBER_FLAG_LEADER_ELIGIBLE : 0U) |
        (member.spatial_profile.committed ? MEMBER_FLAG_SPATIAL_PROFILE : 0U);
    std::memcpy(output + offset, member.participant_id, id_length);
    offset += id_length;
    if (member.spatial_profile.committed) {
      const std::size_t space_length =
          std::strlen(member.spatial_profile.space_id);
      const std::size_t group_length =
          std::strlen(member.spatial_profile.group_id);
      output[offset++] = static_cast<uint8_t>(space_length);
      output[offset++] = static_cast<uint8_t>(group_length);
      std::memcpy(output + offset, member.spatial_profile.space_id,
                  space_length);
      offset += space_length;
      std::memcpy(output + offset, member.spatial_profile.group_id,
                  group_length);
      offset += group_length;
      write_u32(output + offset, member.spatial_profile.pose_version);
      offset += 4;
      write_u32(output + offset,
                member.spatial_profile.calibration_version);
      offset += 4;
      write_i32(output + offset,
                member.spatial_profile.transform.origin_x_mm);
      offset += 4;
      write_i32(output + offset,
                member.spatial_profile.transform.origin_y_mm);
      offset += 4;
      write_i16(output + offset,
                member.spatial_profile.transform.rotation_cdeg);
      offset += 2;
    }
  }
  write_u32(output + offset, fnv1a(output, offset));
  *output_size = offset + CHECKSUM_SIZE;
  return SiteConfigurationCodecResult::OK;
}

SiteConfigurationCodecResult SiteConfigurationCodec::decode(
    const uint8_t *data, std::size_t size,
    SiteConfigurationSnapshot *snapshot) {
  if (data == nullptr || snapshot == nullptr)
    return SiteConfigurationCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF))
    return SiteConfigurationCodecResult::EMPTY;
  if (size < LEGACY_HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, CONFIG_MAGIC, sizeof(CONFIG_MAGIC)) != 0)
    return SiteConfigurationCodecResult::CORRUPT;
  const uint16_t storage_version = read_u16(data + 4);
  if (storage_version != LEGACY_STORAGE_VERSION &&
      storage_version != SPATIAL_PROFILE_STORAGE_VERSION &&
      storage_version != GROUP_ASSIGNMENT_STORAGE_VERSION &&
      storage_version != STORAGE_VERSION)
    return SiteConfigurationCodecResult::UNSUPPORTED_VERSION;

  const std::size_t member_count = data[6];
  const uint8_t flags = data[7];
  const std::size_t site_length = data[16];
  const std::size_t owner_length = data[17];
  const std::size_t floorplan_length = data[18];
  if (member_count == 0 || member_count > ParticipantRegistry::MAX_PARTICIPANTS ||
      (flags & ~(FLAG_ROSTER_COMMITTED | FLAG_FLOORPLAN_COMMITTED)) != 0 ||
      data[19] != 0 || site_length == 0 ||
      site_length > PlatformIdentity::MAX_ID_LENGTH ||
      owner_length > ParticipantRecord::MAX_ID_LENGTH ||
      floorplan_length > ParticipantRecord::MAX_ID_LENGTH)
    return SiteConfigurationCodecResult::CORRUPT;

  const std::size_t header_size =
      storage_version >= STORAGE_VERSION ? HEADER_SIZE : LEGACY_HEADER_SIZE;
  std::size_t offset = header_size;
  if (offset + site_length + owner_length + floorplan_length + CHECKSUM_SIZE > size)
    return SiteConfigurationCodecResult::CORRUPT;
  *snapshot = {};
  auto &decoded = *snapshot;
  std::memcpy(decoded.site_id, data + offset, site_length);
  offset += site_length;
  std::memcpy(decoded.configuration_owner_node_id, data + offset, owner_length);
  offset += owner_length;
  std::memcpy(decoded.floorplan.floorplan_id, data + offset, floorplan_length);
  offset += floorplan_length;
  decoded.generation = read_u32(data + 8);
  decoded.roster_revision = read_u32(data + 12);
  decoded.authority_epoch =
      storage_version >= STORAGE_VERSION ? read_u32(data + 32) : 1U;
  decoded.roster_committed = (flags & FLAG_ROSTER_COMMITTED) != 0;
  decoded.floorplan.committed = (flags & FLAG_FLOORPLAN_COMMITTED) != 0;
  decoded.floorplan.geometry_version = read_u32(data + 20);
  decoded.floorplan.config_hash = read_u32(data + 24);
  decoded.floorplan.image_hash = read_u32(data + 28);

  for (std::size_t index = 0; index < member_count; ++index) {
    if (offset + RECORD_HEADER_SIZE + CHECKSUM_SIZE > size)
      return SiteConfigurationCodecResult::CORRUPT;
    const std::size_t id_length = data[offset++];
    auto &member = decoded.members[index];
    member.type = static_cast<ParticipantType>(data[offset++]);
    member.trust = static_cast<ParticipantTrust>(data[offset++]);
    member.membership = static_cast<ParticipantMembership>(data[offset++]);
    const uint8_t member_flags = data[offset++];
    const uint8_t allowed_member_flags =
        storage_version >= 2
            ? MEMBER_FLAG_LEADER_ELIGIBLE | MEMBER_FLAG_SPATIAL_PROFILE
            : MEMBER_FLAG_LEADER_ELIGIBLE;
    const bool has_spatial_profile =
        (member_flags & MEMBER_FLAG_SPATIAL_PROFILE) != 0;
    if (id_length == 0 || id_length > ParticipantRecord::MAX_ID_LENGTH ||
        (member_flags & ~allowed_member_flags) != 0 ||
        offset + id_length +
                (has_spatial_profile
                     ? (storage_version >= 3
                            ? SPATIAL_PROFILE_V3_HEADER_SIZE +
                                  SPATIAL_PROFILE_V2_SIZE
                            : SPATIAL_PROFILE_V2_SIZE)
                     : 0) +
                CHECKSUM_SIZE >
            size)
      return SiteConfigurationCodecResult::CORRUPT;
    member.occupied = true;
    member.leader_eligible =
        (member_flags & MEMBER_FLAG_LEADER_ELIGIBLE) != 0;
    std::memcpy(member.participant_id, data + offset, id_length);
    offset += id_length;
    if (has_spatial_profile) {
      member.spatial_profile.committed = true;
      if (storage_version >= 3) {
        const std::size_t space_length = data[offset++];
        const std::size_t group_length = data[offset++];
        if (space_length > ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
            group_length > ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
            (space_length == 0) != (group_length == 0) ||
            offset + space_length + group_length +
                    SPATIAL_PROFILE_V2_SIZE + CHECKSUM_SIZE >
                size)
          return SiteConfigurationCodecResult::CORRUPT;
        std::memcpy(member.spatial_profile.space_id, data + offset,
                    space_length);
        offset += space_length;
        std::memcpy(member.spatial_profile.group_id, data + offset,
                    group_length);
        offset += group_length;
      }
      member.spatial_profile.pose_version = read_u32(data + offset);
      offset += 4;
      member.spatial_profile.calibration_version = read_u32(data + offset);
      offset += 4;
      member.spatial_profile.transform.origin_x_mm =
          read_i32(data + offset);
      offset += 4;
      member.spatial_profile.transform.origin_y_mm =
          read_i32(data + offset);
      offset += 4;
      member.spatial_profile.transform.rotation_cdeg =
          read_i16(data + offset);
      offset += 2;
      member.spatial_profile.transform.residual_mm = 0;
      member.spatial_profile.transform.sample_count = 1;
      member.spatial_profile.transform.valid = true;
    }
  }
  if (offset + CHECKSUM_SIZE > size ||
      read_u32(data + offset) != fnv1a(data, offset))
    return SiteConfigurationCodecResult::CORRUPT;
  offset += CHECKSUM_SIZE;
  if (offset < size && !all_bytes_are(data + offset, size - offset, 0xFF))
    return SiteConfigurationCodecResult::CORRUPT;
  if (!valid_configuration(decoded))
    return SiteConfigurationCodecResult::CORRUPT;
  return SiteConfigurationCodecResult::OK;
}

SiteConfigurationStoreResult SiteConfigurationStore::decode_slot_() {
  const auto result = SiteConfigurationCodec::decode(
      this->blob_.bytes.data(), this->blob_.bytes.size(), &this->scratch_);
  if (result == SiteConfigurationCodecResult::OK)
    return SiteConfigurationStoreResult::LOADED;
  if (result == SiteConfigurationCodecResult::EMPTY)
    return SiteConfigurationStoreResult::EMPTY;
  if (result == SiteConfigurationCodecResult::UNSUPPORTED_VERSION)
    return SiteConfigurationStoreResult::UNSUPPORTED_VERSION;
  return SiteConfigurationStoreResult::CORRUPT;
}

SiteConfigurationStoreResult SiteConfigurationStore::load_committed_(
    SiteConfigurationStorageBackend &backend, uint8_t *active_slot) {
  SiteConfigurationCommitBlob commit{};
  if (backend.load_site_configuration_commit(&commit)) {
    uint8_t slot = 0;
    uint32_t generation = 0;
    const auto commit_result = decode_commit(commit, &slot, &generation);
    if (commit_result != SiteConfigurationStoreResult::LOADED)
      return SiteConfigurationStoreResult::CORRUPT;
    if (!backend.load_site_configuration_slot(slot, &this->blob_) ||
        this->decode_slot_() != SiteConfigurationStoreResult::LOADED ||
        this->scratch_.generation != generation)
      return SiteConfigurationStoreResult::CORRUPT;
    if (active_slot != nullptr)
      *active_slot = slot;
    return SiteConfigurationStoreResult::LOADED;
  }

  int valid_slot = -1;
  uint32_t valid_generation = 0;
  bool present = false;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (!backend.load_site_configuration_slot(slot, &this->blob_))
      continue;
    present = true;
    if (this->decode_slot_() != SiteConfigurationStoreResult::LOADED)
      continue;
    if (valid_slot >= 0)
      return SiteConfigurationStoreResult::CORRUPT;
    valid_slot = slot;
    valid_generation = this->scratch_.generation;
  }
  if (!present)
    return SiteConfigurationStoreResult::EMPTY;
  if (valid_slot < 0 ||
      !backend.load_site_configuration_slot(static_cast<uint8_t>(valid_slot),
                                             &this->blob_) ||
      this->decode_slot_() != SiteConfigurationStoreResult::LOADED ||
      this->scratch_.generation != valid_generation)
    return SiteConfigurationStoreResult::CORRUPT;

  SiteConfigurationCommitBlob repaired{};
  encode_commit(static_cast<uint8_t>(valid_slot), valid_generation, &repaired);
  if (!backend.save_site_configuration_commit(repaired) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (active_slot != nullptr)
    *active_slot = static_cast<uint8_t>(valid_slot);
  return SiteConfigurationStoreResult::COMMIT_REPAIRED;
}

SiteConfigurationStoreResult SiteConfigurationStore::load(
    SiteConfigurationStorageBackend &backend,
    SiteConfigurationSnapshot *snapshot) {
  if (snapshot == nullptr)
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  const auto result = this->load_committed_(backend, nullptr);
  if (result == SiteConfigurationStoreResult::LOADED ||
      result == SiteConfigurationStoreResult::COMMIT_REPAIRED)
    *snapshot = this->scratch_;
  return result;
}

SiteConfigurationStoreResult SiteConfigurationStore::write_slot_(
    SiteConfigurationStorageBackend &backend, uint8_t slot) {
  std::size_t encoded_size = 0;
  if (SiteConfigurationCodec::encode(
          this->scratch_, this->blob_.bytes.data(), this->blob_.bytes.size(),
          &encoded_size) != SiteConfigurationCodecResult::OK ||
      encoded_size == 0)
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  if (!backend.save_site_configuration_slot(slot, this->blob_) ||
      !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  return SiteConfigurationStoreResult::UPDATED;
}

SiteConfigurationStoreResult SiteConfigurationStore::create(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  this->scratch_ = configuration;
  this->scratch_.generation = 1;
  const auto write_result = this->write_slot_(backend, 0);
  if (write_result != SiteConfigurationStoreResult::UPDATED)
    return write_result;
  SiteConfigurationCommitBlob commit{};
  encode_commit(0, this->scratch_.generation, &commit);
  if (!backend.save_site_configuration_commit(commit) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return SiteConfigurationStoreResult::CREATED;
}

SiteConfigurationStoreResult SiteConfigurationStore::update(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != SiteConfigurationStoreResult::LOADED &&
      load_result != SiteConfigurationStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (this->scratch_.generation == std::numeric_limits<uint32_t>::max() ||
      configuration.authority_epoch < this->scratch_.authority_epoch ||
      (configuration.authority_epoch != this->scratch_.authority_epoch &&
       (this->scratch_.authority_epoch ==
            std::numeric_limits<uint32_t>::max() ||
        configuration.authority_epoch !=
            this->scratch_.authority_epoch + 1U)))
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  const uint32_t next_generation = this->scratch_.generation + 1;
  this->scratch_ = configuration;
  this->scratch_.generation = next_generation;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != SiteConfigurationStoreResult::UPDATED)
    return write_result;
  SiteConfigurationCommitBlob commit{};
  encode_commit(next_slot, next_generation, &commit);
  if (!backend.save_site_configuration_commit(commit) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return SiteConfigurationStoreResult::UPDATED;
}

SiteConfigurationStoreResult SiteConfigurationStore::replace_for_enrollment(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  if (!valid_configuration(configuration))
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != SiteConfigurationStoreResult::LOADED &&
      load_result != SiteConfigurationStoreResult::COMMIT_REPAIRED)
    return load_result;
  this->scratch_ = configuration;
  this->scratch_.generation = 1;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != SiteConfigurationStoreResult::UPDATED)
    return write_result;
  SiteConfigurationCommitBlob commit{};
  encode_commit(next_slot, this->scratch_.generation, &commit);
  if (!backend.save_site_configuration_commit(commit) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return SiteConfigurationStoreResult::UPDATED;
}

SiteConfigurationStoreResult SiteConfigurationStore::replace_for_local_reset(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  return this->replace_for_enrollment(backend, configuration, snapshot);
}

SiteConfigurationStoreResult SiteConfigurationStore::replace_with_newer(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  if (!valid_configuration(configuration))
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != SiteConfigurationStoreResult::LOADED &&
      load_result != SiteConfigurationStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (compare_site_configuration_revision(configuration, this->scratch_) !=
      SiteConfigurationRevisionOrder::NEWER)
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  this->scratch_ = configuration;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != SiteConfigurationStoreResult::UPDATED)
    return write_result;
  SiteConfigurationCommitBlob commit{};
  encode_commit(next_slot, configuration.generation, &commit);
  if (!backend.save_site_configuration_commit(commit) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return SiteConfigurationStoreResult::UPDATED;
}

SiteConfigurationStoreResult
SiteConfigurationStore::replace_with_quorum_authority(
    SiteConfigurationStorageBackend &backend,
    const SiteConfigurationSnapshot &configuration,
    SiteConfigurationSnapshot *snapshot) {
  if (!valid_configuration(configuration))
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != SiteConfigurationStoreResult::LOADED &&
      load_result != SiteConfigurationStoreResult::COMMIT_REPAIRED)
    return load_result;
  const auto order =
      compare_site_configuration_revision(configuration, this->scratch_);
  if (order == SiteConfigurationRevisionOrder::OLDER ||
      (order == SiteConfigurationRevisionOrder::SAME &&
       same_configuration(configuration, this->scratch_)))
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  this->scratch_ = configuration;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != SiteConfigurationStoreResult::UPDATED)
    return write_result;
  SiteConfigurationCommitBlob commit{};
  encode_commit(next_slot, configuration.generation, &commit);
  if (!backend.save_site_configuration_commit(commit) || !backend.sync())
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return SiteConfigurationStoreResult::UPDATED;
}

const char *site_configuration_store_result_to_string(
    SiteConfigurationStoreResult result) {
  switch (result) {
    case SiteConfigurationStoreResult::EMPTY: return "empty";
    case SiteConfigurationStoreResult::LOADED: return "loaded";
    case SiteConfigurationStoreResult::CREATED: return "created";
    case SiteConfigurationStoreResult::UPDATED: return "updated";
    case SiteConfigurationStoreResult::COMMIT_REPAIRED: return "commit_repaired";
    case SiteConfigurationStoreResult::CORRUPT: return "corrupt";
    case SiteConfigurationStoreResult::UNSUPPORTED_VERSION: return "unsupported_version";
    case SiteConfigurationStoreResult::STORAGE_FAILED: return "storage_failed";
    case SiteConfigurationStoreResult::INVALID_CONFIGURATION: return "invalid_configuration";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

class RadarStorageSiteConfigurationBackend
    : public SiteConfigurationStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return storage != nullptr && storage->ensure_partition();
  }

  bool load_site_configuration_slot(
      uint8_t slot, SiteConfigurationStorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::SITE_CONFIGURATION_SLOT_0
                            : RadarPlatformBlob::SITE_CONFIGURATION_SLOT_1;
    return this->storage_->read_platform_blob(target, blob->bytes.data(),
                                              blob->bytes.size());
  }

  bool save_site_configuration_slot(
      uint8_t slot, const SiteConfigurationStorageBlob &blob) override {
    if (slot >= 2)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::SITE_CONFIGURATION_SLOT_0
                            : RadarPlatformBlob::SITE_CONFIGURATION_SLOT_1;
    return this->storage_->write_platform_blob(target, blob.bytes.data(),
                                               blob.bytes.size());
  }

  bool load_site_configuration_commit(
      SiteConfigurationCommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob(
                                  RadarPlatformBlob::SITE_CONFIGURATION_COMMIT,
                                  blob->bytes.data(), blob->bytes.size());
  }

  bool save_site_configuration_commit(
      const SiteConfigurationCommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::SITE_CONFIGURATION_COMMIT, blob.bytes.data(),
        blob.bytes.size());
  }

  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32SiteConfigurationPersistence {
  RadarStorageSiteConfigurationBackend backend{};
  SiteConfigurationStore *store{nullptr};
  bool store_allocation_attempted{false};

  bool ensure_store() {
    if (this->store != nullptr)
      return true;
    if (this->store_allocation_attempted)
      return false;
    this->store_allocation_attempted = true;

#ifdef USE_PSRAM
    void *memory =
        heap_caps_calloc(1, sizeof(SiteConfigurationStore),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      ESP_LOGE("site_configuration",
               "Store allocation component=site_configuration region="
               "psram_failed size=%u internal_free=%u internal_largest=%u "
               "psram_free=%u psram_largest=%u",
               static_cast<unsigned>(sizeof(SiteConfigurationStore)),
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
    this->store = new (memory) SiteConfigurationStore();
    ESP_LOGI("site_configuration",
             "Store allocation component=site_configuration region=psram "
             "size=%u internal_free=%u psram_free=%u",
             static_cast<unsigned>(sizeof(SiteConfigurationStore)),
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#else
    this->store = new (std::nothrow) SiteConfigurationStore();
    if (this->store == nullptr) {
      ESP_LOGE("site_configuration",
               "Store allocation component=site_configuration region="
               "allocation_failed size=%u",
               static_cast<unsigned>(sizeof(SiteConfigurationStore)));
      return false;
    }
    ESP_LOGW("site_configuration",
             "Store allocation component=site_configuration region=internal "
             "size=%u reason=psram_disabled",
             static_cast<unsigned>(sizeof(SiteConfigurationStore)));
#endif
    return true;
  }
};

static_assert(sizeof(Esp32SiteConfigurationPersistence) <= 32,
              "Site configuration persistence shell must remain small");

Esp32SiteConfigurationPersistence &site_configuration_persistence() {
  static Esp32SiteConfigurationPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_site_configuration_persistence(
    SiteConfigurationStorageBackend **backend, SiteConfigurationStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = site_configuration_persistence();
  if (!persistence.ensure_store()) {
    *backend = nullptr;
    *store = nullptr;
    return false;
  }
  *backend = &persistence.backend;
  *store = persistence.store;
  return true;
}

SiteConfigurationStoreResult initialize_esp32_site_configuration(
    RadarStorage *storage, const PlatformIdentity &identity,
    const ParticipantRegistry &participants, bool enrolled,
    SiteConfigurationSnapshot *snapshot) {
  if (storage == nullptr || snapshot == nullptr)
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  auto &persistence = site_configuration_persistence();
  if (!persistence.backend.initialize(storage))
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  SiteConfigurationStorageBackend *backend = nullptr;
  SiteConfigurationStore *store = nullptr;
  if (!get_esp32_site_configuration_persistence(&backend, &store))
    return SiteConfigurationStoreResult::STORAGE_FAILED;
  const auto load_result = store->load(*backend, snapshot);
  if (load_result != SiteConfigurationStoreResult::EMPTY) {
    if ((load_result == SiteConfigurationStoreResult::LOADED ||
         load_result == SiteConfigurationStoreResult::COMMIT_REPAIRED) &&
        std::strcmp(snapshot->configuration_owner_node_id,
                    identity.node_id) == 0) {
      bool changed = false;
      SiteConfigurationSnapshot reconciled{};
      if (reconcile_unattested_namo_trust(identity, *snapshot, &reconciled,
                                          &changed) !=
          SiteConfigurationResult::OK)
        return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
      if (changed)
        return store->update(*backend, reconciled, snapshot);
    }
    return load_result;
  }
  if (initialize_site_configuration(identity, enrolled, snapshot) !=
      SiteConfigurationResult::OK)
    return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  if (!enrolled && participants.size() > 0) {
    if (commit_local_site_roster(identity, participants, *snapshot, snapshot) !=
        SiteConfigurationResult::OK)
      return SiteConfigurationStoreResult::INVALID_CONFIGURATION;
  }
  return store->create(*backend, *snapshot, snapshot);
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
