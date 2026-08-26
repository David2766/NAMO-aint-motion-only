#include "site_heartbeat.h"

#include "credential_provisioning.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#ifdef USE_ESP32
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <cstdio>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t LEGACY_HEARTBEAT_VERSION = 1;
constexpr uint8_t FLOORPLAN_HEARTBEAT_VERSION = 2;
constexpr uint8_t PREVIOUS_HEARTBEAT_VERSION = 3;
constexpr uint8_t GROUP_SUMMARY_HEARTBEAT_VERSION = 4;
constexpr uint8_t CONFIGURATION_FENCE_HEARTBEAT_VERSION = 5;
constexpr uint8_t SITE_TOPOLOGY_HEARTBEAT_VERSION = 6;
constexpr uint8_t DIRECTORY_FENCE_HEARTBEAT_VERSION = 7;
constexpr uint8_t CREDENTIAL_FENCE_HEARTBEAT_VERSION = 8;
constexpr uint8_t ELECTION_FENCE_HEARTBEAT_VERSION = 9;
constexpr uint8_t STABLE_ELECTION_HEARTBEAT_VERSION = 10;
constexpr uint8_t WINNER_ELECTION_HEARTBEAT_VERSION = 11;
constexpr uint8_t HEARTBEAT_VERSION =
    SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION;
constexpr uint8_t HEARTBEAT_KIND = 2;

void write_u32_be(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value >> 24U);
  target[1] = static_cast<uint8_t>(value >> 16U);
  target[2] = static_cast<uint8_t>(value >> 8U);
  target[3] = static_cast<uint8_t>(value);
}

void write_u64_be(uint8_t *target, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index)
    target[index] = static_cast<uint8_t>(value >> ((7U - index) * 8U));
}

uint32_t read_u32_be(const uint8_t *source) {
  return (static_cast<uint32_t>(source[0]) << 24U) |
         (static_cast<uint32_t>(source[1]) << 16U) |
         (static_cast<uint32_t>(source[2]) << 8U) |
         static_cast<uint32_t>(source[3]);
}

uint64_t read_u64_be(const uint8_t *source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index)
    value = (value << 8U) | source[index];
  return value;
}

bool valid_text(const char *value, std::size_t maximum, bool allow_empty) {
  if (value == nullptr)
    return false;
  const std::size_t length = std::strlen(value);
  if ((!allow_empty && length == 0) || length > maximum)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const char character = value[index];
    const bool alphanumeric =
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9');
    if (!alphanumeric && character != '-' && character != '_' &&
        character != '.' && character != ':')
      return false;
  }
  return true;
}

bool copy_packet_text(const uint8_t *source, std::size_t length, char *target,
                      std::size_t capacity, bool allow_empty) {
  if (source == nullptr || target == nullptr || length >= capacity ||
      (!allow_empty && length == 0))
    return false;
  std::memcpy(target, source, length);
  target[length] = '\0';
  return valid_text(target, capacity - 1, allow_empty);
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

}  // namespace

const char *resolve_expected_group_leader(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache &endpoints,
    const char *site_leader_node_id, const char *group_id,
    uint32_t now_ms) {
  uint8_t member_count = 0;
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE &&
        member.spatial_profile.committed &&
        std::strcmp(member.spatial_profile.group_id, group_id) == 0)
      ++member_count;
  }
  const auto *directory_group =
      directory != nullptr && directory->committed
          ? directory->find_group(group_id)
          : nullptr;
  const bool exact_directory_shape =
      group_directory_has_exact_shape(directory_group, member_count);
  const bool expanding_directory_shape =
      group_directory_is_expanding(directory_group, member_count);
  if (exact_directory_shape || expanding_directory_shape) {
    const auto *assigned =
        configuration.find(directory_group->leader_node_id);
    const bool assigned_local =
        std::strcmp(directory_group->leader_node_id, identity.node_id) == 0;
    const bool assigned_online =
        assigned_local || ParticipantEndpointCache::is_online(
                              endpoints.find(directory_group->leader_node_id),
                              now_ms);
    if (assigned != nullptr &&
        assigned->membership == ParticipantMembership::ACTIVE &&
        assigned->type == ParticipantType::NAMO_NODE &&
        assigned->leader_eligible && assigned->spatial_profile.committed &&
        std::strcmp(assigned->spatial_profile.group_id, group_id) == 0 &&
        group_leader_assignment_allowed(
            configuration, group_id, site_leader_node_id,
            assigned->participant_id) &&
        assigned_online)
      return assigned->participant_id;
  }

  const SiteConfigurationMember *selected = nullptr;
  uint8_t selected_penalty = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE ||
        !member.leader_eligible || !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.group_id, group_id) != 0)
      continue;
    const bool local =
        std::strcmp(member.participant_id, identity.node_id) == 0;
    if (!local && !ParticipantEndpointCache::is_online(
                      endpoints.find(member.participant_id), now_ms))
      continue;
    const uint8_t penalty = group_leader_role_penalty(
        &configuration, site_leader_node_id, member.participant_id);
    if (selected == nullptr ||
        leader_candidate_preferred(member.participant_id, penalty,
                                   selected->participant_id,
                                   selected_penalty)) {
      selected = &member;
      selected_penalty = penalty;
    }
  }
  return selected != nullptr ? selected->participant_id : nullptr;
}

SiteHeartbeatCodecResult encode_site_heartbeat(
    const SiteHeartbeatMessage &message, uint32_t key_version,
    uint64_t participant_token, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size,
    bool compatibility_v4, bool compatibility_v5, bool compatibility_v6,
    bool compatibility_v7, bool compatibility_v8, bool compatibility_v9,
    bool compatibility_v10, bool compatibility_v11) {
  if (packet == nullptr || signed_size == nullptr || packet_size == nullptr ||
      static_cast<uint8_t>(compatibility_v4) +
              static_cast<uint8_t>(compatibility_v5) +
              static_cast<uint8_t>(compatibility_v6) +
              static_cast<uint8_t>(compatibility_v7) +
              static_cast<uint8_t>(compatibility_v8) +
              static_cast<uint8_t>(compatibility_v9) +
              static_cast<uint8_t>(compatibility_v10) +
              static_cast<uint8_t>(compatibility_v11) >
          1 ||
      key_version == 0 || participant_token == 0 || message.sequence == 0 ||
      message.site_term == 0 ||
      message.lease_ms < SiteCoordinator::MIN_LEASE_MS ||
      message.lease_ms > SiteCoordinator::MAX_LEASE_MS ||
      message.site_configuration_generation == 0 ||
      (!compatibility_v4 &&
       (message.site_configuration_authority_epoch == 0 ||
        message.site_configuration_digest == 0)) ||
      (!compatibility_v4 && !compatibility_v5 &&
       (message.site_topology_revision == 0 ||
        message.site_topology_digest == 0)) ||
      (!compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
       (message.site_directory_revision == 0 ||
        message.site_directory_digest == 0)) ||
      (!compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
       !compatibility_v7 && !compatibility_v8 &&
       (message.site_election_view_digest == 0 ||
        (message.group_summary_present &&
         message.group_election_view_digest == 0))) ||
      (message.floorplan_owner_advertisement && !compatibility_v4 &&
       !compatibility_v5 &&
       (message.floorplan_id[0] == '\0' ||
        message.site_configuration_generation == 0 ||
        message.floorplan_geometry_version == 0 ||
        message.floorplan_config_hash == 0 ||
        message.floorplan_image_hash == 0)) ||
      (message.floorplan_replica_ready &&
       (message.site_configuration_generation == 0 ||
        message.floorplan_geometry_version == 0 ||
        message.floorplan_config_hash == 0 ||
        message.floorplan_image_hash == 0)) ||
      (message.group_summary_present &&
       (message.group_id[0] == '\0' || message.group_term == 0 ||
        message.group_assignment_revision == 0 ||
        message.group_participants_expected == 0 ||
        message.group_participants_expected >
            SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ||
        message.group_participants_online >
            message.group_participants_expected)) ||
      !valid_text(message.participant_id, ParticipantRecord::MAX_ID_LENGTH,
                  false) ||
      !valid_text(message.site_id, PlatformIdentity::MAX_ID_LENGTH, false) ||
      !valid_text(message.group_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH, true) ||
      !valid_text(message.boot_id, GroupHeartbeat::MAX_BOOT_ID_LENGTH, false) ||
      !valid_text(message.floorplan_id, ParticipantRecord::MAX_ID_LENGTH,
                  true))
    return SiteHeartbeatCodecResult::INVALID_ARGUMENT;

  const std::size_t participant_size = std::strlen(message.participant_id);
  const std::size_t site_size = std::strlen(message.site_id);
  const std::size_t group_size = std::strlen(message.group_id);
  const std::size_t boot_size = std::strlen(message.boot_id);
  const std::size_t floorplan_size =
      compatibility_v4 || compatibility_v5
          ? 0
          : std::strlen(message.floorplan_id);
  const std::size_t body_size = participant_size + site_size + group_size +
                                boot_size + floorplan_size;
  const std::size_t prefix_size =
      compatibility_v4 ? SiteHeartbeatEnvelope::GROUP_SUMMARY_PREFIX_SIZE
                       : (compatibility_v5
                              ? SiteHeartbeatEnvelope::CONFIGURATION_FENCE_PREFIX_SIZE
                              : (compatibility_v6
                                      ? SiteHeartbeatEnvelope::SITE_TOPOLOGY_PREFIX_SIZE
                                      : (compatibility_v7 || compatibility_v8
                                             ? SiteHeartbeatEnvelope::CREDENTIAL_FENCE_PREFIX_SIZE
                                             : SiteHeartbeatEnvelope::PREFIX_SIZE)));
  const std::size_t signed_length = prefix_size + body_size;
  const std::size_t total_size = signed_length + SiteHeartbeatEnvelope::TAG_SIZE;
  if (total_size > SiteHeartbeatEnvelope::MAX_PACKET_SIZE ||
      total_size > packet_capacity)
    return SiteHeartbeatCodecResult::BUFFER_TOO_SMALL;

  std::memset(packet, 0, total_size);
  std::memcpy(packet, "NMHB", 4);
  packet[4] = compatibility_v4
                  ? GROUP_SUMMARY_HEARTBEAT_VERSION
                  : (compatibility_v5
                         ? CONFIGURATION_FENCE_HEARTBEAT_VERSION
                         : (compatibility_v6
                                ? SITE_TOPOLOGY_HEARTBEAT_VERSION
                                 : (compatibility_v7
                                        ? DIRECTORY_FENCE_HEARTBEAT_VERSION
                                        : (compatibility_v8
                                               ? CREDENTIAL_FENCE_HEARTBEAT_VERSION
                                                : (compatibility_v9
                                                       ? ELECTION_FENCE_HEARTBEAT_VERSION
                                                       : (compatibility_v10
                                                              ? STABLE_ELECTION_HEARTBEAT_VERSION
                                                              : (compatibility_v11
                                                                     ? WINNER_ELECTION_HEARTBEAT_VERSION
                                                                     : HEARTBEAT_VERSION)))))));
  packet[5] = HEARTBEAT_KIND;
  packet[6] = static_cast<uint8_t>(prefix_size);
  write_u32_be(packet + 8, key_version);
  write_u64_be(packet + 12, participant_token);
  write_u32_be(packet + 20, message.sequence);
  write_u32_be(packet + 24, message.site_term);
  write_u32_be(packet + 28, message.group_term);
  write_u32_be(packet + 32, message.lease_ms);
  write_u32_be(packet + 36, message.site_configuration_generation);
  write_u32_be(packet + 40, message.floorplan_geometry_version);
  write_u32_be(packet + 44, message.floorplan_config_hash);
  write_u32_be(packet + 48, message.floorplan_image_hash);
  write_u32_be(packet + 52, message.radar_pose_version);
  packet[56] = message.floorplan_replica_ready ? 1 : 0;
  const std::size_t lengths_offset =
      compatibility_v4
          ? 68
          : (compatibility_v5
                 ? 84
                 : (compatibility_v6
                        ? 108
                        : (compatibility_v7 || compatibility_v8 ? 124
                                                                : 132)));
  if (message.group_summary_present) {
    packet[57] = 1;
    packet[58] = message.group_participants_expected;
    packet[59] = message.group_participants_online;
    write_u32_be(packet + 60, message.group_assignment_revision);
  }
  if (!compatibility_v4) {
    write_u32_be(packet + 72,
                 message.site_configuration_authority_epoch);
    write_u64_be(packet + 76, message.site_configuration_digest);
  }
  if (!compatibility_v4 && !compatibility_v5) {
    write_u32_be(packet + 88, message.site_topology_revision);
    write_u64_be(packet + 92, message.site_topology_digest);
    packet[100] = message.floorplan_owner_advertisement ? 1 : 0;
    packet[101] = static_cast<uint8_t>(floorplan_size);
  }
  if (!compatibility_v4 && !compatibility_v5 && !compatibility_v6) {
    write_u32_be(packet + 108, message.site_directory_revision);
    write_u32_be(packet + 112, message.site_directory_digest);
    if (!compatibility_v7)
      packet[116] = message.site_credentials_complete ? 1 : 0;
  }
  if (!compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
      !compatibility_v7 && !compatibility_v8) {
    write_u32_be(packet + 124, message.site_election_view_digest);
    write_u32_be(packet + 128, message.group_election_view_digest);
  }
  packet[lengths_offset] = static_cast<uint8_t>(participant_size);
  packet[lengths_offset + 1] = static_cast<uint8_t>(site_size);
  packet[lengths_offset + 2] = static_cast<uint8_t>(group_size);
  packet[lengths_offset + 3] = static_cast<uint8_t>(boot_size);
  std::size_t offset = prefix_size;
  for (const auto item : std::array<std::pair<const char *, std::size_t>, 5>{
           std::make_pair(message.participant_id, participant_size),
           std::make_pair(message.site_id, site_size),
           std::make_pair(message.group_id, group_size),
           std::make_pair(message.boot_id, boot_size),
           std::make_pair(message.floorplan_id, floorplan_size)}) {
    std::memcpy(packet + offset, item.first, item.second);
    offset += item.second;
  }
  *signed_size = signed_length;
  *packet_size = total_size;
  return SiteHeartbeatCodecResult::OK;
}

SiteHeartbeatCodecResult parse_site_heartbeat(
    const uint8_t *packet, std::size_t packet_size,
    SiteHeartbeatEnvelope *envelope) {
  if (packet == nullptr || envelope == nullptr ||
      packet_size < SiteHeartbeatEnvelope::LEGACY_PREFIX_SIZE +
                        SiteHeartbeatEnvelope::TAG_SIZE + 4 ||
      packet_size > SiteHeartbeatEnvelope::MAX_PACKET_SIZE ||
      std::memcmp(packet, "NMHB", 4) != 0 ||
      packet[5] != HEARTBEAT_KIND || packet[7] != 0)
    return SiteHeartbeatCodecResult::INVALID_PACKET;

  const bool legacy = packet[4] == LEGACY_HEARTBEAT_VERSION;
  const bool floorplan_version = packet[4] == FLOORPLAN_HEARTBEAT_VERSION;
  const bool previous_version = packet[4] == PREVIOUS_HEARTBEAT_VERSION;
  const bool group_summary_version =
      packet[4] == GROUP_SUMMARY_HEARTBEAT_VERSION;
  const bool configuration_fence_version =
      packet[4] == CONFIGURATION_FENCE_HEARTBEAT_VERSION;
  const bool site_topology_version =
      packet[4] == SITE_TOPOLOGY_HEARTBEAT_VERSION;
  const bool directory_fence_version =
      packet[4] == DIRECTORY_FENCE_HEARTBEAT_VERSION;
  const bool credential_fence_version =
      packet[4] == CREDENTIAL_FENCE_HEARTBEAT_VERSION;
  const bool election_fence_version =
      packet[4] == ELECTION_FENCE_HEARTBEAT_VERSION;
  const bool stable_election_version =
      packet[4] == STABLE_ELECTION_HEARTBEAT_VERSION;
  const bool winner_election_version =
      packet[4] == WINNER_ELECTION_HEARTBEAT_VERSION;
  const std::size_t prefix_size =
      legacy ? SiteHeartbeatEnvelope::LEGACY_PREFIX_SIZE
             : (floorplan_version
                    ? SiteHeartbeatEnvelope::FLOORPLAN_PREFIX_SIZE
                    : (previous_version
                           ? SiteHeartbeatEnvelope::PREVIOUS_PREFIX_SIZE
                           : (group_summary_version
                                  ? SiteHeartbeatEnvelope::GROUP_SUMMARY_PREFIX_SIZE
                                  : (configuration_fence_version
                                         ? SiteHeartbeatEnvelope::CONFIGURATION_FENCE_PREFIX_SIZE
                                         : (site_topology_version
                                                 ? SiteHeartbeatEnvelope::SITE_TOPOLOGY_PREFIX_SIZE
                                                 : (directory_fence_version ||
                                                            credential_fence_version
                                                        ? SiteHeartbeatEnvelope::CREDENTIAL_FENCE_PREFIX_SIZE
                                                        : SiteHeartbeatEnvelope::PREFIX_SIZE))))));
  if ((!legacy && !floorplan_version && !previous_version &&
       !group_summary_version && !configuration_fence_version &&
       !site_topology_version && !directory_fence_version &&
       !credential_fence_version && !election_fence_version &&
       !stable_election_version && !winner_election_version &&
       packet[4] != HEARTBEAT_VERSION) ||
      packet[6] != prefix_size ||
      packet_size < prefix_size + SiteHeartbeatEnvelope::TAG_SIZE + 4)
    return SiteHeartbeatCodecResult::INVALID_PACKET;

  const std::size_t lengths_offset =
      legacy ? 36
             : (floorplan_version
                    ? 56
                    : (previous_version
                           ? 60
                           : (group_summary_version
                                  ? 68
                                  : (configuration_fence_version
                                         ? 84
                                          : (site_topology_version
                                                 ? 108
                                                 : (directory_fence_version ||
                                                            credential_fence_version
                                                        ? 124
                                                        : 132))))));
  const std::size_t participant_size = packet[lengths_offset];
  const std::size_t site_size = packet[lengths_offset + 1];
  const std::size_t group_size = packet[lengths_offset + 2];
  const std::size_t boot_size = packet[lengths_offset + 3];
  const std::size_t floorplan_size =
      (packet[4] == HEARTBEAT_VERSION || winner_election_version ||
       stable_election_version ||
       election_fence_version ||
       directory_fence_version ||
       credential_fence_version ||
       site_topology_version)
          ? packet[101]
          : 0;
  const std::size_t body_size = participant_size + site_size + group_size +
                                boot_size + floorplan_size;
  const std::size_t signed_size = prefix_size + body_size;
  if (participant_size == 0 || site_size == 0 || boot_size == 0 ||
      packet_size != signed_size + SiteHeartbeatEnvelope::TAG_SIZE)
    return SiteHeartbeatCodecResult::INVALID_PACKET;

  SiteHeartbeatEnvelope parsed;
  parsed.wire_version = packet[4];
  parsed.key_version = read_u32_be(packet + 8);
  parsed.participant_token = read_u64_be(packet + 12);
  parsed.message.sequence = read_u32_be(packet + 20);
  parsed.message.site_term = read_u32_be(packet + 24);
  parsed.message.group_term = read_u32_be(packet + 28);
  parsed.message.lease_ms = read_u32_be(packet + 32);
  if (!legacy) {
    parsed.message.site_configuration_generation = read_u32_be(packet + 36);
    parsed.message.floorplan_geometry_version = read_u32_be(packet + 40);
    parsed.message.floorplan_config_hash = read_u32_be(packet + 44);
    parsed.message.floorplan_image_hash = read_u32_be(packet + 48);
    const std::size_t ready_offset = floorplan_version ? 52 : 56;
    if (!floorplan_version)
      parsed.message.radar_pose_version = read_u32_be(packet + 52);
    if (packet[ready_offset] > 1)
      return SiteHeartbeatCodecResult::INVALID_PACKET;
    if (floorplan_version &&
        (packet[53] != 0 || packet[54] != 0 || packet[55] != 0))
      return SiteHeartbeatCodecResult::INVALID_PACKET;
    if (previous_version &&
        (packet[57] != 0 || packet[58] != 0 || packet[59] != 0))
      return SiteHeartbeatCodecResult::INVALID_PACKET;
    parsed.message.floorplan_replica_ready = packet[ready_offset] == 1;
    if (parsed.message.floorplan_replica_ready &&
        (parsed.message.site_configuration_generation == 0 ||
         parsed.message.floorplan_geometry_version == 0 ||
         parsed.message.floorplan_config_hash == 0 ||
         parsed.message.floorplan_image_hash == 0))
      return SiteHeartbeatCodecResult::INVALID_PACKET;
    if (!floorplan_version && !previous_version) {
      if (packet[57] > 1 || packet[64] != 0 || packet[65] != 0 ||
          packet[66] != 0 || packet[67] != 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
      parsed.message.group_summary_present = packet[57] == 1;
      parsed.message.group_participants_expected = packet[58];
      parsed.message.group_participants_online = packet[59];
      parsed.message.group_assignment_revision = read_u32_be(packet + 60);
      if ((!parsed.message.group_summary_present &&
           (parsed.message.group_participants_expected != 0 ||
            parsed.message.group_participants_online != 0 ||
            parsed.message.group_assignment_revision != 0)) ||
          (parsed.message.group_summary_present &&
           (parsed.message.group_term == 0 ||
            parsed.message.group_assignment_revision == 0 ||
            parsed.message.group_participants_expected == 0 ||
            parsed.message.group_participants_expected >
                SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ||
            parsed.message.group_participants_online >
                parsed.message.group_participants_expected)))
        return SiteHeartbeatCodecResult::INVALID_PACKET;
    }
    if (!group_summary_version && !previous_version && !floorplan_version) {
      parsed.message.site_configuration_authority_epoch =
          read_u32_be(packet + 72);
      parsed.message.site_configuration_digest = read_u64_be(packet + 76);
      if (parsed.message.site_configuration_authority_epoch == 0 ||
          parsed.message.site_configuration_digest == 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
    }
    if (site_topology_version || directory_fence_version ||
        election_fence_version ||
        stable_election_version || winner_election_version ||
        packet[4] == HEARTBEAT_VERSION) {
      if (packet[100] > 1 || packet[102] != 0 || packet[103] != 0 ||
          packet[104] != 0 || packet[105] != 0 || packet[106] != 0 ||
          packet[107] != 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
      parsed.message.site_topology_revision = read_u32_be(packet + 88);
      parsed.message.site_topology_digest = read_u64_be(packet + 92);
      parsed.message.floorplan_owner_advertisement = packet[100] == 1;
      if (parsed.message.site_topology_revision == 0 ||
          parsed.message.site_topology_digest == 0 ||
          floorplan_size > ParticipantRecord::MAX_ID_LENGTH ||
          (parsed.message.floorplan_owner_advertisement &&
           floorplan_size == 0))
        return SiteHeartbeatCodecResult::INVALID_PACKET;
    }
    if (directory_fence_version || credential_fence_version ||
        election_fence_version ||
        stable_election_version || winner_election_version ||
        packet[4] == HEARTBEAT_VERSION) {
      if ((directory_fence_version && packet[116] != 0) ||
          (!directory_fence_version && packet[116] > 1) ||
          packet[117] != 0 || packet[118] != 0 ||
          packet[119] != 0 || packet[120] != 0 || packet[121] != 0 ||
          packet[122] != 0 || packet[123] != 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
      parsed.message.site_directory_revision = read_u32_be(packet + 108);
      parsed.message.site_directory_digest = read_u32_be(packet + 112);
      if (parsed.message.site_directory_revision == 0 ||
          parsed.message.site_directory_digest == 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
      parsed.message.site_credentials_complete =
          !directory_fence_version && packet[116] == 1;
    }
    if (election_fence_version || stable_election_version ||
        winner_election_version ||
        packet[4] == HEARTBEAT_VERSION) {
      parsed.message.site_election_view_digest = read_u32_be(packet + 124);
      parsed.message.group_election_view_digest = read_u32_be(packet + 128);
      if (parsed.message.site_election_view_digest == 0)
        return SiteHeartbeatCodecResult::INVALID_PACKET;
    }
  }
  std::size_t offset = prefix_size;
  if (parsed.key_version == 0 || parsed.participant_token == 0 ||
      parsed.message.sequence == 0 || parsed.message.site_term == 0 ||
      parsed.message.lease_ms < SiteCoordinator::MIN_LEASE_MS ||
      parsed.message.lease_ms > SiteCoordinator::MAX_LEASE_MS ||
      !copy_packet_text(packet + offset, participant_size,
                        parsed.message.participant_id,
                        sizeof(parsed.message.participant_id), false))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  offset += participant_size;
  if (!copy_packet_text(packet + offset, site_size, parsed.message.site_id,
                        sizeof(parsed.message.site_id), false))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  offset += site_size;
  if (!copy_packet_text(packet + offset, group_size, parsed.message.group_id,
                        sizeof(parsed.message.group_id), true))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  if (parsed.message.group_summary_present &&
      parsed.message.group_id[0] == '\0')
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  offset += group_size;
  if (!copy_packet_text(packet + offset, boot_size, parsed.message.boot_id,
                        sizeof(parsed.message.boot_id), false))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  offset += boot_size;
  if (!copy_packet_text(packet + offset, floorplan_size,
                        parsed.message.floorplan_id,
                        sizeof(parsed.message.floorplan_id), true))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  if (parsed.message.floorplan_owner_advertisement &&
      (parsed.message.floorplan_id[0] == '\0' ||
       parsed.message.floorplan_geometry_version == 0 ||
       parsed.message.floorplan_config_hash == 0 ||
       parsed.message.floorplan_image_hash == 0))
    return SiteHeartbeatCodecResult::INVALID_PACKET;
  parsed.signed_data = packet;
  parsed.signed_size = signed_size;
  parsed.tag = packet + signed_size;
  *envelope = parsed;
  return SiteHeartbeatCodecResult::OK;
}

const char *site_heartbeat_codec_result_to_string(
    SiteHeartbeatCodecResult result) {
  switch (result) {
    case SiteHeartbeatCodecResult::OK: return "ok";
    case SiteHeartbeatCodecResult::INVALID_ARGUMENT: return "invalid_argument";
    case SiteHeartbeatCodecResult::BUFFER_TOO_SMALL: return "buffer_too_small";
    case SiteHeartbeatCodecResult::INVALID_PACKET: return "invalid_packet";
  }
  return "invalid_packet";
}

ParticipantEndpoint *ParticipantEndpointCache::ensure_(
    const char *participant_id) {
  if (!valid_text(participant_id, ParticipantRecord::MAX_ID_LENGTH, false))
    return nullptr;
  ParticipantEndpoint *empty = nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
    if (!entry.occupied && empty == nullptr)
      empty = &entry;
  }
  if (empty == nullptr)
    return nullptr;
  *empty = ParticipantEndpoint{};
  empty->occupied = true;
  copy_text(empty->participant_id, sizeof(empty->participant_id),
            participant_id);
  return empty;
}

bool ParticipantEndpointCache::observe_discovery(
    const char *participant_id, uint32_t ipv4, uint16_t port,
    uint32_t now_ms) {
  if (ipv4 == 0 || port == 0)
    return false;
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  if (entry->authenticated) {
    if (entry->ipv4 == ipv4 && entry->port == port) {
      entry->discovered_at_ms = now_ms;
      return false;
    }
    if (!time_reached_(now_ms, entry->authenticated_at_ms +
                                   AUTHENTICATED_ADDRESS_HOLD_MS))
      return false;
  }
  const bool changed = entry->ipv4 != ipv4 || entry->port != port;
  entry->ipv4 = ipv4;
  entry->port = port;
  entry->discovered_at_ms = now_ms;
  entry->authenticated = false;
  return changed;
}

bool ParticipantEndpointCache::observe_authenticated(
    const char *participant_id, uint32_t ipv4, uint16_t port,
    uint32_t now_ms) {
  if (ipv4 == 0 || port == 0)
    return false;
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  const bool changed = entry->ipv4 != ipv4 || entry->port != port;
  entry->ipv4 = ipv4;
  entry->port = port;
  entry->authenticated = true;
  entry->authenticated_at_ms = now_ms;
  return changed;
}

bool ParticipantEndpointCache::observe_floorplan_replica(
    const char *participant_id,
    uint32_t site_configuration_authority_epoch,
    uint32_t site_configuration_generation,
    uint64_t site_configuration_digest, uint32_t geometry_version,
    uint32_t config_hash, uint32_t image_hash, uint32_t radar_pose_version,
    bool ready) {
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  const bool fenced_revision =
      site_configuration_authority_epoch != 0 &&
      site_configuration_digest != 0;
  if (fenced_revision || entry->site_configuration_authority_epoch == 0) {
    entry->site_configuration_authority_epoch =
        site_configuration_authority_epoch;
    entry->site_configuration_generation = site_configuration_generation;
    entry->site_configuration_digest = site_configuration_digest;
  } else if (site_configuration_generation !=
             entry->site_configuration_generation) {
    entry->site_configuration_authority_epoch = 0;
    entry->site_configuration_generation = site_configuration_generation;
    entry->site_configuration_digest = 0;
  }
  entry->floorplan_geometry_version = geometry_version;
  entry->floorplan_config_hash = config_hash;
  entry->floorplan_image_hash = image_hash;
  entry->radar_pose_version = radar_pose_version;
  entry->floorplan_replica_ready = ready;
  return true;
}

bool ParticipantEndpointCache::observe_site_topology(
    const char *participant_id, uint32_t topology_revision,
    uint64_t topology_digest) {
  if (topology_revision == 0 || topology_digest == 0)
    return false;
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  entry->site_topology_revision = topology_revision;
  entry->site_topology_digest = topology_digest;
  return true;
}

bool ParticipantEndpointCache::observe_site_directory(
    const char *participant_id, uint32_t directory_revision,
    uint32_t directory_digest) {
  if (directory_revision == 0 || directory_digest == 0)
    return false;
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  entry->site_directory_revision = directory_revision;
  entry->site_directory_digest = directory_digest;
  return true;
}

bool ParticipantEndpointCache::observe_site_credentials(
    const char *participant_id, uint8_t heartbeat_wire_version,
    bool credentials_complete) {
  auto *entry = this->ensure_(participant_id);
  if (entry == nullptr)
    return false;
  entry->latest_heartbeat_wire_version = heartbeat_wire_version;
  if (heartbeat_wire_version < ELECTION_FENCE_HEARTBEAT_VERSION) {
    if (entry->heartbeat_wire_version < ELECTION_FENCE_HEARTBEAT_VERSION) {
      entry->heartbeat_wire_version = heartbeat_wire_version;
      entry->site_credentials_complete = false;
    }
    return true;
  }
  if (heartbeat_wire_version > entry->heartbeat_wire_version)
    entry->heartbeat_wire_version = heartbeat_wire_version;
  entry->site_credentials_complete = credentials_complete;
  return true;
}

bool participant_endpoint_matches_configuration(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration) {
  return endpoint != nullptr &&
         endpoint->site_configuration_authority_epoch ==
             configuration.authority_epoch &&
         endpoint->site_configuration_generation == configuration.generation &&
         endpoint->site_configuration_digest ==
             site_configuration_digest(configuration);
}

bool participant_endpoint_is_bounded_roster_transition(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, uint8_t heartbeat_wire_version) {
  if (endpoint == nullptr || !configuration.roster_committed ||
      !site_configuration_member_active(configuration, participant_id) ||
      heartbeat_wire_version < SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION ||
      !endpoint->site_credentials_complete ||
      endpoint->site_configuration_authority_epoch == 0 ||
      endpoint->site_configuration_generation == 0 ||
      endpoint->site_configuration_digest == 0 ||
      endpoint->site_topology_revision == 0 ||
      endpoint->site_topology_digest == 0 ||
      endpoint->site_configuration_authority_epoch !=
          configuration.authority_epoch ||
      configuration.generation == 0 || configuration.roster_revision == 0)
    return false;

  const bool peer_is_previous =
      endpoint->site_configuration_generation <
          std::numeric_limits<uint32_t>::max() &&
      endpoint->site_configuration_generation + 1U ==
          configuration.generation &&
      endpoint->site_topology_revision <
          std::numeric_limits<uint32_t>::max() &&
      endpoint->site_topology_revision + 1U ==
          configuration.roster_revision;
  const bool peer_is_next =
      configuration.generation < std::numeric_limits<uint32_t>::max() &&
      configuration.generation + 1U ==
          endpoint->site_configuration_generation &&
      configuration.roster_revision <
          std::numeric_limits<uint32_t>::max() &&
      configuration.roster_revision + 1U ==
          endpoint->site_topology_revision;
  return peer_is_previous || peer_is_next;
}

bool participant_endpoint_matches_coordination_fence(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, uint8_t heartbeat_wire_version) {
  if (endpoint == nullptr)
    return false;
  if (endpoint->site_topology_revision == 0)
    return participant_endpoint_matches_configuration(endpoint,
                                                       configuration);
  return (endpoint->site_topology_revision == configuration.roster_revision &&
          endpoint->site_topology_digest ==
              site_topology_digest(configuration)) ||
         participant_endpoint_is_bounded_roster_transition(
             endpoint, configuration, participant_id,
             heartbeat_wire_version);
}

bool participant_endpoint_matches_site_authority(
    const ParticipantEndpoint *endpoint, uint32_t authority_epoch,
    uint32_t generation, uint64_t configuration_digest,
    uint32_t topology_revision, uint64_t topology_digest) {
  return endpoint != nullptr && authority_epoch != 0 && generation != 0 &&
         configuration_digest != 0 && topology_revision != 0 &&
         topology_digest != 0 &&
         endpoint->site_configuration_authority_epoch == authority_epoch &&
         endpoint->site_configuration_generation == generation &&
         endpoint->site_configuration_digest == configuration_digest &&
         endpoint->site_topology_revision == topology_revision &&
         endpoint->site_topology_digest == topology_digest;
}

bool automatic_handover_authority_available(
    const ParticipantEndpoint *configuration_owner_endpoint, uint32_t now_ms,
    uint32_t authority_epoch, uint32_t generation,
    uint64_t configuration_digest, uint32_t topology_revision,
    uint64_t topology_digest) {
  return !ParticipantEndpointCache::is_online(configuration_owner_endpoint,
                                              now_ms) ||
         participant_endpoint_matches_site_authority(
             configuration_owner_endpoint, authority_epoch, generation,
             configuration_digest, topology_revision, topology_digest);
}

void ParticipantEndpointCache::reconcile(
    const ParticipantCredentialRegistry &credentials) {
  for (auto &entry : this->entries_) {
    if (entry.occupied && credentials.find(entry.participant_id) == nullptr)
      entry = ParticipantEndpoint{};
  }
}

void ParticipantEndpointCache::reset_authentication() {
  for (auto &entry : this->entries_) {
    if (!entry.occupied)
      continue;
    entry.authenticated = false;
    entry.authenticated_at_ms = 0;
  }
}

const ParticipantEndpoint *ParticipantEndpointCache::find(
    const char *participant_id) const {
  if (!valid_text(participant_id, ParticipantRecord::MAX_ID_LENGTH, false))
    return nullptr;
  for (const auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

const ParticipantEndpoint *ParticipantEndpointCache::endpoint_at(
    std::size_t slot) const {
  return slot < this->entries_.size() && this->entries_[slot].occupied
             ? &this->entries_[slot]
             : nullptr;
}

std::size_t ParticipantEndpointCache::size() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_)
    count += entry.occupied ? 1U : 0U;
  return count;
}

bool site_heartbeat_peer_known(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    const char *participant_id) {
  const auto *participant = participants.find(participant_id);
  if (participant != nullptr)
    return participant->membership != ParticipantMembership::REVOKED;
  return configuration != nullptr && configuration->roster_committed &&
         site_configuration_member_active(*configuration, participant_id);
}

bool ParticipantEndpointCache::is_online(const ParticipantEndpoint *endpoint,
                                          uint32_t now_ms) {
  return endpoint != nullptr && endpoint->authenticated &&
         !time_reached_(now_ms,
                        endpoint->authenticated_at_ms + ONLINE_TIMEOUT_MS);
}

bool ParticipantEndpointCache::needs_discovery_refresh(
    const ParticipantCredentialRegistry &credentials,
    const ParticipantRegistry &participants, uint32_t now_ms) const {
  return this->needs_discovery_refresh(credentials, participants, nullptr,
                                       now_ms);
}

bool ParticipantEndpointCache::needs_discovery_refresh(
    const ParticipantCredentialRegistry &credentials,
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    uint32_t now_ms) const {
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *credential = credentials.record_at(slot);
    if (credential == nullptr)
      continue;
    if (!site_heartbeat_peer_known(participants, configuration,
                                   credential->participant_id))
      continue;
    if (!is_online(this->find(credential->participant_id), now_ms))
      return true;
  }
  return false;
}

bool ParticipantEndpointCache::time_reached_(uint32_t now_ms,
                                             uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool SiteHeartbeatSequenceTracker::accept(uint64_t participant_token,
                                          const char *boot_id,
                                          uint32_t sequence) {
  if (participant_token == 0 || sequence == 0 ||
      !valid_text(boot_id, GroupHeartbeat::MAX_BOOT_ID_LENGTH, false))
    return false;
  Entry *target = nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied && entry.participant_token == participant_token) {
      target = &entry;
      break;
    }
    if (!entry.occupied && target == nullptr)
      target = &entry;
  }
  if (target == nullptr)
    return false;
  const bool same_boot =
      target->occupied && std::strcmp(target->boot_id, boot_id) == 0;
  if (same_boot && sequence <= target->sequence)
    return false;
  target->occupied = true;
  target->participant_token = participant_token;
  copy_text(target->boot_id, sizeof(target->boot_id), boot_id);
  target->sequence = sequence;
  return true;
}

void SiteHeartbeatSequenceTracker::reconcile(
    const ParticipantCredentialRegistry &credentials) {
  for (auto &entry : this->entries_) {
    if (!entry.occupied)
      continue;
    bool found = false;
    for (std::size_t slot = 0;
         slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
      const auto *credential = credentials.record_at(slot);
      if (credential != nullptr &&
          credential->participant_token == entry.participant_token) {
        found = true;
        break;
      }
    }
    if (!found)
      entry = Entry{};
  }
}

const char *site_heartbeat_result_to_string(SiteHeartbeatResult result) {
  switch (result) {
    case SiteHeartbeatResult::IDLE: return "idle";
    case SiteHeartbeatResult::OK: return "ok";
    case SiteHeartbeatResult::INVALID_PACKET: return "invalid_packet";
    case SiteHeartbeatResult::UNKNOWN_KEY: return "unknown_key";
    case SiteHeartbeatResult::AUTHENTICATION_FAILED: return "authentication_failed";
    case SiteHeartbeatResult::IDENTITY_MISMATCH: return "identity_mismatch";
    case SiteHeartbeatResult::DUPLICATE_OR_REORDERED: return "duplicate_or_reordered";
    case SiteHeartbeatResult::SITE_MISMATCH: return "site_mismatch";
    case SiteHeartbeatResult::PARTICIPANT_REJECTED: return "participant_rejected";
    case SiteHeartbeatResult::CONFIGURATION_CONFLICT: return "configuration_conflict";
    case SiteHeartbeatResult::SITE_REJECTED: return "site_rejected";
    case SiteHeartbeatResult::GROUP_REJECTED: return "group_rejected";
    case SiteHeartbeatResult::SOCKET_ERROR: return "socket_error";
  }
  return "unknown";
}

SiteHeartbeatAuthorityProjection project_site_heartbeat_authority(
    const SiteCoordinatorState &state, bool control_plane_bootstrap) {
  if (control_plane_bootstrap)
    return {1U, 0U, 0U, 0U, true};
  return {state.term, state.election_view_digest,
          state.compatibility_v11_election_view_digest,
          state.previous_election_view_digest, false};
}

bool site_heartbeat_current_wire_probe(uint32_t send_cycle_sequence) {
  return send_cycle_sequence != 0 &&
         send_cycle_sequence % SITE_HEARTBEAT_CURRENT_WIRE_PROBE_INTERVAL == 0;
}

#ifdef USE_ESP32

namespace {

static const char *const TAG = "site_heartbeat";

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                         std::size_t size) {
  uint8_t difference = 0;
  for (std::size_t index = 0; index < size; ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

}  // namespace

SiteHeartbeatTransport::SiteHeartbeatTransport() = default;

SiteHeartbeatTransport::~SiteHeartbeatTransport() {
  this->destroy_socket_();
}

bool SiteHeartbeatTransport::setup(
    ParticipantCredentialRegistry *credentials,
    ParticipantDiscoveryRuntime *discovery, const PlatformIdentity &identity) {
  this->credentials_ = credentials;
  this->discovery_ = discovery;
  this->diagnostics_.port = PORT;
  if (credentials == nullptr || discovery == nullptr ||
      identity.node_id[0] == '\0') {
    ESP_LOGE(TAG,
             "Heartbeat setup failed stage=dependencies credentials=%s "
             "discovery=%s identity=%s",
             credentials != nullptr ? "ready" : "missing",
             discovery != nullptr ? "ready" : "missing",
             identity.node_id[0] != '\0' ? "ready" : "missing");
    return false;
  }

  uint32_t random_values[2]{};
  if (!esphome::random_bytes(reinterpret_cast<uint8_t *>(random_values),
                             sizeof(random_values))) {
    ESP_LOGE(TAG,
             "Heartbeat setup failed stage=boot_id reason=random_source");
    return false;
  }
  std::snprintf(this->boot_id_, sizeof(this->boot_id_), "%08x%08x",
                static_cast<unsigned>(random_values[0]),
                static_cast<unsigned>(random_values[1]));
  return true;
}

void SiteHeartbeatTransport::loop(
    const PlatformIdentity &identity, const SiteMembership &membership,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory,
    const SiteFloorplanManifest *local_floorplan,
    uint32_t local_configuration_generation,
    uint32_t local_radar_pose_version, bool local_floorplan_replica_ready,
    bool outbound_identity_ready,
    SiteCoordinator *site,
    GroupCoordinator *groups,
    SiteGroupSummaryCache *site_group_summaries,
    SiteFloorplanOwnerCache *site_floorplan_owners,
    uint32_t now_ms) {
  if (site_group_summaries != nullptr) {
    const bool local_authority =
        site != nullptr && site->state().local_leader;
    site_group_summaries->reconcile(directory, local_authority, now_ms);
    if (local_authority && groups != nullptr && directory != nullptr &&
        configuration != nullptr) {
      const auto *local_group =
          groups->local_group(configuration, identity.node_id);
      const auto *directory_group =
          local_group != nullptr
              ? directory->find_group(local_group->group_id)
              : nullptr;
      const char *floorplan_id =
          directory_group != nullptr
              ? directory_group->floorplan_id
              : (local_floorplan != nullptr && local_floorplan->committed
                     ? local_floorplan->floorplan_id
                     : nullptr);
      const char *expected_leader =
          local_group != nullptr
              ? resolve_expected_group_leader(
                    identity, *configuration, directory, this->endpoints_,
                    site->state().leader_node_id, local_group->group_id, now_ms)
              : nullptr;
      if (local_group != nullptr && local_group->configured &&
          local_group->local_leader && floorplan_id != nullptr &&
          expected_leader != nullptr &&
          std::strcmp(expected_leader, identity.node_id) == 0) {
        const auto *current =
            site_group_summaries->find(local_group->group_id);
        const bool refresh =
            current == nullptr ||
            std::strcmp(current->leader_node_id, identity.node_id) != 0 ||
            std::strcmp(current->floorplan_id, floorplan_id) != 0 ||
            current->term != local_group->term ||
            current->participants_expected !=
                local_group->participants_expected ||
            current->participants_online != local_group->participants_online ||
            SiteGroupSummaryCache::lease_remaining_ms(*current, now_ms) <=
                HEARTBEAT_INTERVAL_MS;
        if (refresh) {
          SiteGroupSummaryReport report{};
          report.participant_id = identity.node_id;
          report.group_id = local_group->group_id;
          report.assignment_revision =
              directory_group != nullptr
                  ? directory_group->assignment_revision
                  : local_group->term;
          report.term = local_group->term;
          report.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
          report.participants_expected = local_group->participants_expected;
          report.participants_online = local_group->participants_online;
          site_group_summaries->accept_discovered(
              *configuration, directory, expected_leader, floorplan_id, report,
              now_ms);
        }
      }
    }
  }
  if (site_floorplan_owners != nullptr) {
    const bool local_authority =
        site != nullptr && site->state().local_leader;
    site_floorplan_owners->reconcile(configuration, local_authority, now_ms);
    if (local_authority && configuration != nullptr &&
        local_floorplan != nullptr && local_floorplan->committed &&
        std::strcmp(configuration->configuration_owner_node_id,
                    identity.node_id) == 0) {
      SiteFloorplanOwnerReport report{};
      report.participant_id = identity.node_id;
      report.floorplan_id = local_floorplan->floorplan_id;
      report.owner_generation = local_configuration_generation;
      report.geometry_version = local_floorplan->geometry_version;
      report.config_hash = local_floorplan->config_hash;
      report.image_hash = local_floorplan->image_hash;
      report.lease_ms = SiteCoordinator::DEFAULT_LEASE_MS;
      site_floorplan_owners->accept(*configuration, report, now_ms);
    }
  }
  if (this->credentials_ == nullptr || this->discovery_ == nullptr ||
      site == nullptr || groups == nullptr || !network::is_connected() ||
      !this->ensure_socket_(now_ms))
    return;
  this->endpoints_.reconcile(*this->credentials_);
  this->sequences_.reconcile(*this->credentials_);
  this->ingest_discovery_(now_ms);
  const bool discovery_refresh_needed =
      this->endpoints_.needs_discovery_refresh(
          *this->credentials_, participants, configuration, now_ms);
  if (!discovery_refresh_needed) {
    this->next_discovery_ms_ = 0;
  } else if (this->next_discovery_ms_ == 0 ||
             time_reached(now_ms, this->next_discovery_ms_)) {
    this->discovery_->queue_refresh(identity.node_id);
    this->next_discovery_ms_ = now_ms + DISCOVERY_REFRESH_MS;
  }
  this->receive_(identity, participants, readiness, configuration, directory,
                 site, groups, site_group_summaries, site_floorplan_owners,
                 now_ms);
  if (outbound_identity_ready) {
    this->send_(identity, membership, participants, readiness, configuration,
                directory, local_floorplan, local_configuration_generation,
                local_radar_pose_version, local_floorplan_replica_ready,
                outbound_identity_ready, *site, *groups, now_ms);
  } else {
    this->send_cycle_active_ = false;
    this->send_cursor_ = 0;
  }
  this->update_diagnostics_(now_ms);
}

bool SiteHeartbeatTransport::ensure_socket_(uint32_t now_ms) {
  if (this->socket_ != nullptr)
    return true;
  if (this->next_socket_attempt_ms_ != 0 &&
      !time_reached(now_ms, this->next_socket_attempt_ms_))
    return false;
  this->next_socket_attempt_ms_ = now_ms + SOCKET_RETRY_MS;

  this->socket_ =
      socket::socket_ip_loop_monitored(SOCK_DGRAM, IPPROTO_UDP).release();
  if (this->socket_ == nullptr || this->socket_->setblocking(false) != 0) {
    ESP_LOGE(TAG,
             "Heartbeat socket failed stage=create_or_nonblocking errno=%d",
             errno);
    this->diagnostics_.last_result = SiteHeartbeatResult::SOCKET_ERROR;
    this->destroy_socket_();
    return false;
  }
  sockaddr_storage server{};
  const socklen_t server_size = socket::set_sockaddr_any(
      reinterpret_cast<sockaddr *>(&server), sizeof(server), PORT);
  if (server_size == 0 ||
      this->socket_->bind(reinterpret_cast<sockaddr *>(&server), server_size) !=
          0) {
    ESP_LOGE(TAG, "Heartbeat socket failed stage=bind port=%u errno=%d",
             static_cast<unsigned>(PORT), errno);
    this->diagnostics_.last_result = SiteHeartbeatResult::SOCKET_ERROR;
    this->destroy_socket_();
    return false;
  }
  this->next_socket_attempt_ms_ = 0;
  this->diagnostics_.socket_ready = true;
  ESP_LOGI(TAG, "Authenticated Site heartbeat listening on UDP %u",
           static_cast<unsigned>(PORT));
  return true;
}

void SiteHeartbeatTransport::destroy_socket_() {
  delete this->socket_;
  this->socket_ = nullptr;
  this->diagnostics_.socket_ready = false;
}

void SiteHeartbeatTransport::ingest_discovery_(uint32_t now_ms) {
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    ParticipantDiscoveryCandidate candidate{};
    if (!this->discovery_->candidate_at(slot, &candidate) ||
        this->credentials_->find(candidate.participant_id) == nullptr)
      continue;
    in_addr address{};
    if (inet_pton(AF_INET, candidate.address, &address) != 1)
      continue;
    if (this->endpoints_.observe_discovery(
            candidate.participant_id, address.s_addr, PORT, now_ms))
      this->diagnostics_.address_changes++;
  }
}

void SiteHeartbeatTransport::receive_(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, SiteCoordinator *site,
    GroupCoordinator *groups, SiteGroupSummaryCache *site_group_summaries,
    SiteFloorplanOwnerCache *site_floorplan_owners,
    uint32_t now_ms) {
  if (!this->drain_pending_ && !this->socket_->ready())
    return;
  this->drain_pending_ = false;
  for (uint8_t index = 0; index < MAX_PACKETS_PER_LOOP; ++index) {
    this->ram_checkpoint_.record("site_heartbeat_receive", "entry");
    sockaddr_storage sender{};
    socklen_t sender_size = sizeof(sender);
    const ssize_t received = this->socket_->recvfrom(
        this->packet_.data(), this->packet_.size(),
        reinterpret_cast<sockaddr *>(&sender), &sender_size);
    this->ram_checkpoint_.record("site_heartbeat_receive", "socket");
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        this->diagnostics_.last_result = SiteHeartbeatResult::SOCKET_ERROR;
      return;
    }
    if (sender.ss_family != AF_INET) {
      this->diagnostics_.rejected++;
      this->diagnostics_.last_result = SiteHeartbeatResult::INVALID_PACKET;
      continue;
    }
    const auto *source = reinterpret_cast<const sockaddr_in *>(&sender);
    const auto result = this->process_packet_(
        this->packet_.data(), static_cast<std::size_t>(received),
        source->sin_addr.s_addr, ntohs(source->sin_port), identity,
        participants, readiness, configuration, directory, site, groups,
        site_group_summaries, site_floorplan_owners, now_ms);
    this->ram_checkpoint_.record("site_heartbeat_receive", "complete");
    this->diagnostics_.last_result = result;
    if (result != SiteHeartbeatResult::OK)
      this->diagnostics_.rejected++;
    if (index + 1 == MAX_PACKETS_PER_LOOP)
      this->drain_pending_ = true;
  }
}

void SiteHeartbeatTransport::send_(
    const PlatformIdentity &identity, const SiteMembership &membership,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory,
    const SiteFloorplanManifest *local_floorplan,
    uint32_t local_configuration_generation,
    uint32_t local_radar_pose_version,
    bool local_floorplan_replica_ready,
    bool outbound_identity_ready,
    const SiteCoordinator &site, const GroupCoordinator &groups,
    uint32_t now_ms) {
  if (configuration == nullptr || !outbound_identity_ready)
    return;
  if (!this->send_cycle_active_) {
    if (this->next_send_ms_ != 0 && !time_reached(now_ms, this->next_send_ms_))
      return;
    this->send_cycle_active_ = true;
    this->send_cursor_ = 0;
    this->send_cycle_sequence_++;
    if (this->send_cycle_sequence_ == 0)
      this->send_cycle_sequence_++;
    this->current_wire_probe_ =
        site_heartbeat_current_wire_probe(this->send_cycle_sequence_);
    this->next_send_ms_ = now_ms + HEARTBEAT_INTERVAL_MS;
  }

  const auto *local_group =
      groups.local_group(configuration, identity.node_id);
  const bool group_summary_ready =
      local_group != nullptr && local_group->configured &&
      local_group->local_leader && local_group->term != 0 &&
      local_group->participants_expected > 0 &&
      local_group->participants_expected <=
          SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS &&
      local_group->participants_online <=
          local_group->participants_expected;
  const bool local_site_credentials_complete =
      site_credentials_complete_for_node(
          identity, *configuration, directory, site.state().leader_node_id,
          *this->credentials_);
  const bool control_plane_bootstrap =
      site_credential_bootstrap_ready_for_node(
          identity, *configuration, participants, *this->credentials_);
  const auto authority = project_site_heartbeat_authority(
      site.state(), control_plane_bootstrap);

  uint8_t processed = 0;
  while (this->send_cursor_ < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
         processed < MAX_PACKETS_PER_LOOP) {
    const auto *credential =
        this->credentials_->record_at(this->send_cursor_++);
    if (credential == nullptr)
      continue;
    processed++;
    if (!site_heartbeat_peer_known(participants, configuration,
                                   credential->participant_id))
      continue;
    const auto *endpoint = this->endpoints_.find(credential->participant_id);
    if (endpoint == nullptr || endpoint->ipv4 == 0)
      continue;
    const auto *sender_credential = select_site_sender_credential(
        identity, configuration, *this->credentials_,
        credential->participant_id);
    if (sender_credential == nullptr || !sender_credential->enabled)
      continue;

    SiteHeartbeatMessage message{};
    copy_text(message.participant_id, sizeof(message.participant_id),
              identity.node_id);
    copy_text(message.site_id, sizeof(message.site_id), membership.site_id);
    copy_text(message.boot_id, sizeof(message.boot_id), this->boot_id_);
    message.sequence = ++this->sequence_;
    if (message.sequence == 0)
      message.sequence = ++this->sequence_;
    message.site_term = authority.site_term;
    message.lease_ms = SiteCoordinator::DEFAULT_LEASE_MS;
    message.site_configuration_authority_epoch =
        configuration->authority_epoch;
    message.site_configuration_generation = local_configuration_generation;
    message.site_configuration_digest =
        site_configuration_digest(*configuration);
    message.site_topology_revision = configuration->roster_revision;
    message.site_topology_digest = site_topology_digest(*configuration);
    if (directory != nullptr && directory->committed &&
        !directory->compatibility_mirror &&
        directory->topology_revision != 0) {
      message.site_directory_revision = directory->topology_revision;
      message.site_directory_digest = site_directory_payload_hash(*directory);
    }
    message.site_credentials_complete = local_site_credentials_complete;
    message.site_election_view_digest = authority.election_view_digest;
    message.radar_pose_version = local_radar_pose_version;
    if (local_floorplan != nullptr && local_floorplan->committed) {
      copy_text(message.floorplan_id, sizeof(message.floorplan_id),
                local_floorplan->floorplan_id);
      message.floorplan_geometry_version = local_floorplan->geometry_version;
      message.floorplan_config_hash = local_floorplan->config_hash;
      message.floorplan_image_hash = local_floorplan->image_hash;
      message.floorplan_replica_ready = local_floorplan_replica_ready;
      message.floorplan_owner_advertisement =
          std::strcmp(configuration->configuration_owner_node_id,
                      identity.node_id) == 0;
    }

    const auto *profile = readiness.find(identity.node_id);
    const char *local_group_id = nullptr;
    if (configuration != nullptr) {
      const auto *local_member = configuration->find(identity.node_id);
      if (local_member != nullptr &&
          local_member->spatial_profile.committed &&
          local_member->spatial_profile.group_id[0] != '\0')
        local_group_id = local_member->spatial_profile.group_id;
    }
    if (local_group_id == nullptr &&
        ParticipantReadinessRegistry::ready(profile))
      local_group_id = profile->config.group_id;
    if (local_group_id != nullptr && local_group_id[0] != '\0') {
      copy_text(message.group_id, sizeof(message.group_id),
                local_group_id);
      message.group_term = 1;
      for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
        const auto *group = groups.group_at(slot);
        if (group != nullptr && group->configured &&
            std::strcmp(group->group_id, message.group_id) == 0) {
          message.group_term = group->term;
          message.group_election_view_digest = group->election_view_digest;
          break;
        }
      }
    }
    const bool compatibility_v4 =
        endpoint->site_configuration_authority_epoch == 0 &&
        !this->current_wire_probe_;
    const bool compatibility_v5 =
        !compatibility_v4 && endpoint->site_topology_revision == 0 &&
        !this->current_wire_probe_;
    const bool compatibility_v6 =
        !compatibility_v4 && !compatibility_v5 &&
        (message.site_directory_revision == 0 ||
         (endpoint->site_directory_revision == 0 &&
          !this->current_wire_probe_));
    const bool compatibility_v7 =
        !compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
        endpoint->heartbeat_wire_version < CREDENTIAL_FENCE_HEARTBEAT_VERSION &&
        !this->current_wire_probe_;
    const bool compatibility_v8 =
        !compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
        !compatibility_v7 &&
        (authority.control_plane_only ||
         (endpoint->heartbeat_wire_version < ELECTION_FENCE_HEARTBEAT_VERSION &&
          !this->current_wire_probe_));
    const bool compatibility_v9 =
        !compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
        !compatibility_v7 && !compatibility_v8 &&
        endpoint->heartbeat_wire_version < STABLE_ELECTION_HEARTBEAT_VERSION &&
        !this->current_wire_probe_;
    const bool compatibility_v10 =
        !compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
        !compatibility_v7 && !compatibility_v8 && !compatibility_v9 &&
        endpoint->heartbeat_wire_version < WINNER_ELECTION_HEARTBEAT_VERSION &&
        !this->current_wire_probe_;
    const bool compatibility_v11 =
        !compatibility_v4 && !compatibility_v5 && !compatibility_v6 &&
        !compatibility_v7 && !compatibility_v8 && !compatibility_v9 &&
        !compatibility_v10 &&
        endpoint->heartbeat_wire_version < HEARTBEAT_VERSION &&
        !this->current_wire_probe_;
    if (compatibility_v9) {
      message.site_election_view_digest =
          authority.previous_election_view_digest;
      if (local_group != nullptr)
        message.group_election_view_digest =
            local_group->legacy_election_view_digest;
    } else if (compatibility_v10) {
      message.site_election_view_digest =
          authority.previous_election_view_digest;
      if (local_group != nullptr)
        message.group_election_view_digest =
            local_group->previous_election_view_digest;
    } else if (compatibility_v11) {
      message.site_election_view_digest =
          authority.compatibility_v11_election_view_digest;
      if (local_group != nullptr)
        message.group_election_view_digest =
            local_group->compatibility_v11_election_view_digest;
    }
    if (!this->send_message_(message, *sender_credential, *endpoint,
                             compatibility_v4, compatibility_v5,
                             compatibility_v6, compatibility_v7,
                              compatibility_v8, compatibility_v9,
                              compatibility_v10, compatibility_v11))
      continue;
    this->diagnostics_.sent++;

    if (group_summary_ready &&
        std::strcmp(credential->participant_id,
                    site.state().leader_node_id) == 0) {
      message.sequence = ++this->sequence_;
      if (message.sequence == 0)
        message.sequence = ++this->sequence_;
      copy_text(message.group_id, sizeof(message.group_id),
                local_group->group_id);
      message.group_term = local_group->term;
      // The ordinary heartbeat above may have substituted a compatibility
      // digest. Rebase the summary on the current contract before selecting
      // the summary packet's independently computed wire version.
      message.site_election_view_digest = authority.election_view_digest;
      message.group_election_view_digest = local_group->election_view_digest;
      message.group_summary_present = true;
      const auto *directory_group =
          directory != nullptr ? directory->find_group(local_group->group_id)
                               : nullptr;
      message.group_assignment_revision =
          directory_group != nullptr &&
                  std::strcmp(directory_group->leader_node_id,
                              identity.node_id) == 0
              ? directory_group->assignment_revision
              : local_group->term;
      message.group_participants_expected =
          local_group->participants_expected;
      message.group_participants_online =
          local_group->participants_online;
      const bool summary_compatibility_v4 =
          endpoint->site_configuration_authority_epoch == 0 &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v5 =
          !summary_compatibility_v4 &&
          endpoint->site_topology_revision == 0 &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v6 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          (message.site_directory_revision == 0 ||
           (endpoint->site_directory_revision == 0 &&
            !this->current_wire_probe_));
      const bool summary_compatibility_v7 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          !summary_compatibility_v6 &&
          endpoint->heartbeat_wire_version < CREDENTIAL_FENCE_HEARTBEAT_VERSION &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v8 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          !summary_compatibility_v6 && !summary_compatibility_v7 &&
          endpoint->heartbeat_wire_version < ELECTION_FENCE_HEARTBEAT_VERSION &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v9 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          !summary_compatibility_v6 && !summary_compatibility_v7 &&
          !summary_compatibility_v8 &&
          endpoint->heartbeat_wire_version < STABLE_ELECTION_HEARTBEAT_VERSION &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v10 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          !summary_compatibility_v6 && !summary_compatibility_v7 &&
          !summary_compatibility_v8 && !summary_compatibility_v9 &&
          endpoint->heartbeat_wire_version <
              WINNER_ELECTION_HEARTBEAT_VERSION &&
          !this->current_wire_probe_;
      const bool summary_compatibility_v11 =
          !summary_compatibility_v4 && !summary_compatibility_v5 &&
          !summary_compatibility_v6 && !summary_compatibility_v7 &&
          !summary_compatibility_v8 && !summary_compatibility_v9 &&
          !summary_compatibility_v10 &&
          endpoint->heartbeat_wire_version < HEARTBEAT_VERSION &&
          !this->current_wire_probe_;
      if (summary_compatibility_v9) {
        message.site_election_view_digest =
            authority.previous_election_view_digest;
        message.group_election_view_digest =
            local_group->legacy_election_view_digest;
      } else if (summary_compatibility_v10) {
        message.site_election_view_digest =
            authority.previous_election_view_digest;
        message.group_election_view_digest =
            local_group->previous_election_view_digest;
      } else if (summary_compatibility_v11) {
        message.site_election_view_digest =
            authority.compatibility_v11_election_view_digest;
        message.group_election_view_digest =
            local_group->compatibility_v11_election_view_digest;
      }
      if (this->send_message_(message, *sender_credential, *endpoint,
                              summary_compatibility_v4,
                              summary_compatibility_v5,
                              summary_compatibility_v6,
                              summary_compatibility_v7,
                               summary_compatibility_v8,
                               summary_compatibility_v9,
                               summary_compatibility_v10,
                               summary_compatibility_v11))
        this->diagnostics_.sent++;
    }
  }
  if (this->send_cursor_ >= ParticipantRegistry::MAX_REMOTE_PARTICIPANTS)
    this->send_cycle_active_ = false;
}

bool SiteHeartbeatTransport::send_message_(
    const SiteHeartbeatMessage &message,
    const ParticipantCredentialRecord &credential,
    const ParticipantEndpoint &endpoint, bool compatibility_v4,
    bool compatibility_v5, bool compatibility_v6, bool compatibility_v7,
    bool compatibility_v8, bool compatibility_v9,
    bool compatibility_v10, bool compatibility_v11) {
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  this->ram_checkpoint_.record("site_heartbeat_send", "entry");
  const bool encoded =
      encode_site_heartbeat(
          message, credential.key_version, credential.participant_token,
          this->packet_.data(), this->packet_.size(), &signed_size,
           &packet_size, compatibility_v4, compatibility_v5, compatibility_v6,
            compatibility_v7, compatibility_v8, compatibility_v9,
            compatibility_v10, compatibility_v11) ==
      SiteHeartbeatCodecResult::OK;
  this->ram_checkpoint_.record("site_heartbeat_send", "encoded");
  const bool authenticated =
      encoded &&
      this->hmac_.calculate(credential.key, this->packet_.data(), signed_size,
                            this->packet_.data() + signed_size);
  this->ram_checkpoint_.record("site_heartbeat_send", "hmac");
  if (!authenticated)
    return false;

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(endpoint.port);
  target.sin_addr.s_addr = endpoint.ipv4;
  const bool sent = this->socket_->sendto(
                        this->packet_.data(), packet_size, 0,
                        reinterpret_cast<sockaddr *>(&target),
                        sizeof(target)) >= 0;
  this->ram_checkpoint_.record("site_heartbeat_send", "socket");
  return sent;
}

SiteHeartbeatResult SiteHeartbeatTransport::process_packet_(
    const uint8_t *packet, std::size_t packet_size, uint32_t source_ipv4,
    uint16_t source_port, const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, SiteCoordinator *site,
    GroupCoordinator *groups, SiteGroupSummaryCache *site_group_summaries,
    SiteFloorplanOwnerCache *site_floorplan_owners,
    uint32_t now_ms) {
  this->diagnostics_.received++;
  this->ram_checkpoint_.record("site_heartbeat_packet", "entry");
  SiteHeartbeatEnvelope envelope{};
  if (parse_site_heartbeat(packet, packet_size, &envelope) !=
      SiteHeartbeatCodecResult::OK)
    return SiteHeartbeatResult::INVALID_PACKET;
  this->ram_checkpoint_.record("site_heartbeat_packet", "envelope");
  const auto *credential = this->credentials_->resolve_provisioned(
      envelope.participant_token, envelope.key_version);
  if (credential == nullptr)
    return SiteHeartbeatResult::UNKNOWN_KEY;
  std::array<uint8_t, SiteHeartbeatEnvelope::TAG_SIZE> expected{};
  const bool authenticated =
      this->hmac_.calculate(credential->key, envelope.signed_data,
                            envelope.signed_size, expected.data()) &&
      constant_time_equal(expected.data(), envelope.tag, expected.size());
  this->ram_checkpoint_.record("site_heartbeat_packet", "hmac");
  if (!authenticated)
    return SiteHeartbeatResult::AUTHENTICATION_FAILED;
  const auto *configuration_owner_endpoint =
      configuration == nullptr
          ? nullptr
          : this->endpoints_.find(
                configuration->configuration_owner_node_id);
  const bool handover_bootstrap =
      configuration != nullptr &&
      automatic_handover_authority_available(
          configuration_owner_endpoint, now_ms,
          envelope.message.site_configuration_authority_epoch,
          envelope.message.site_configuration_generation,
          envelope.message.site_configuration_digest,
          envelope.message.site_topology_revision,
          envelope.message.site_topology_digest) &&
      automatic_configuration_handover_bootstrap_allowed(
          *configuration, credential->participant_id,
          envelope.message.participant_id,
          envelope.message.site_configuration_authority_epoch,
          envelope.message.site_configuration_generation,
          envelope.message.site_configuration_digest,
          envelope.message.site_topology_revision,
          envelope.message.site_topology_digest);
  if (std::strcmp(credential->participant_id,
                  envelope.message.participant_id) != 0 &&
      !handover_bootstrap)
    return SiteHeartbeatResult::IDENTITY_MISMATCH;
  const char *peer_participant_id = handover_bootstrap
                                        ? envelope.message.participant_id
                                        : credential->participant_id;
  this->diagnostics_.authenticated++;
  if (!this->sequences_.accept(envelope.participant_token,
                               envelope.message.boot_id,
                               envelope.message.sequence))
    return SiteHeartbeatResult::DUPLICATE_OR_REORDERED;
  if (std::strcmp(envelope.message.site_id, identity.site_id) != 0)
    return SiteHeartbeatResult::SITE_MISMATCH;
  const auto *participant = participants.find(peer_participant_id);
  if (!site_heartbeat_peer_known(participants, configuration,
                                 peer_participant_id))
    return SiteHeartbeatResult::PARTICIPANT_REJECTED;
  if (this->endpoints_.observe_authenticated(
          peer_participant_id, source_ipv4,
          source_port == 0 ? PORT : source_port, now_ms))
    this->diagnostics_.address_changes++;
  this->endpoints_.observe_floorplan_replica(
      peer_participant_id,
      envelope.message.site_configuration_authority_epoch,
      envelope.message.site_configuration_generation,
      envelope.message.site_configuration_digest,
      envelope.message.floorplan_geometry_version,
      envelope.message.floorplan_config_hash,
      envelope.message.floorplan_image_hash,
      envelope.message.radar_pose_version,
      envelope.message.floorplan_replica_ready);
  if (envelope.message.site_topology_revision != 0)
    this->endpoints_.observe_site_topology(
        peer_participant_id,
        envelope.message.site_topology_revision,
        envelope.message.site_topology_digest);
  if (envelope.message.site_directory_revision != 0)
    this->endpoints_.observe_site_directory(
        peer_participant_id,
        envelope.message.site_directory_revision,
        envelope.message.site_directory_digest);
  this->endpoints_.observe_site_credentials(
      peer_participant_id, envelope.wire_version,
      envelope.message.site_credentials_complete);
  const auto *observed_endpoint =
      this->endpoints_.find(peer_participant_id);
  const bool topology_fenced =
      observed_endpoint != nullptr &&
      observed_endpoint->site_topology_revision != 0;
  const bool bounded_roster_transition =
      configuration != nullptr && topology_fenced &&
      participant_endpoint_is_bounded_roster_transition(
          observed_endpoint, *configuration, peer_participant_id,
          envelope.wire_version);
  if (configuration != nullptr &&
      !participant_endpoint_matches_coordination_fence(
          observed_endpoint, *configuration, peer_participant_id,
          envelope.wire_version))
    return SiteHeartbeatResult::CONFIGURATION_CONFLICT;
  const bool canonical_active =
      configuration != nullptr && configuration->roster_committed &&
      site_configuration_member_active(*configuration,
                                       peer_participant_id);
  if (!canonical_active &&
      (participant == nullptr ||
       participant->membership != ParticipantMembership::ACTIVE))
    return SiteHeartbeatResult::OK;

  SiteLeaderHeartbeat site_heartbeat{};
  site_heartbeat.participant_id = envelope.message.participant_id;
  site_heartbeat.boot_id = envelope.message.boot_id;
  site_heartbeat.sequence = envelope.message.sequence;
  site_heartbeat.term = envelope.message.site_term;
  site_heartbeat.lease_ms = envelope.message.lease_ms;
  site_heartbeat.directory_revision =
      envelope.message.site_directory_revision;
  site_heartbeat.directory_digest = envelope.message.site_directory_digest;
  site_heartbeat.roster_revision = envelope.message.site_topology_revision;
  site_heartbeat.election_view_digest =
      envelope.message.site_election_view_digest;
  site_heartbeat.election_contract_version =
      envelope.wire_version >= HEARTBEAT_VERSION
          ? 3U
          : envelope.wire_version >= WINNER_ELECTION_HEARTBEAT_VERSION ? 2U
                                                                        : 1U;
  site_heartbeat.credentials_complete =
      envelope.message.site_credentials_complete;
  const auto site_accepted =
      site->accept_heartbeat(participants, configuration, site_heartbeat,
                             now_ms);
  if (site_accepted != SiteHeartbeatAdmissionResult::OK &&
      site_accepted !=
          SiteHeartbeatAdmissionResult::DUPLICATE_OR_REORDERED)
    return SiteHeartbeatResult::SITE_REJECTED;
  if (!bounded_roster_transition &&
      envelope.message.floorplan_owner_advertisement &&
      site_floorplan_owners != nullptr && configuration != nullptr &&
      site->state().local_leader) {
    SiteFloorplanOwnerReport report{};
    report.participant_id = envelope.message.participant_id;
    report.floorplan_id = envelope.message.floorplan_id;
    report.owner_generation =
        envelope.message.site_configuration_generation;
    report.geometry_version =
        envelope.message.floorplan_geometry_version;
    report.config_hash = envelope.message.floorplan_config_hash;
    report.image_hash = envelope.message.floorplan_image_hash;
    report.lease_ms = envelope.message.lease_ms;
    if (site_floorplan_owners->accept(*configuration, report, now_ms) !=
        SiteFloorplanOwnerResult::OK)
      return SiteHeartbeatResult::CONFIGURATION_CONFLICT;
  }
  if (envelope.message.group_summary_present) {
    // Summary and floorplan-owner metadata require the exact canonical roster.
    if (!bounded_roster_transition && site_group_summaries != nullptr &&
        directory != nullptr &&
        site->state().local_leader) {
      SiteGroupSummaryReport report{};
      report.participant_id = envelope.message.participant_id;
      report.group_id = envelope.message.group_id;
      report.assignment_revision =
          envelope.message.group_assignment_revision;
      report.term = envelope.message.group_term;
      report.lease_ms = envelope.message.lease_ms;
      report.participants_expected =
          envelope.message.group_participants_expected;
      report.participants_online =
          envelope.message.group_participants_online;
      const auto *directory_group = directory->find_group(report.group_id);
      const bool matches_directory =
          directory_group != nullptr &&
          std::strcmp(directory_group->leader_node_id,
                      report.participant_id) == 0 &&
          directory_group->assignment_revision ==
              report.assignment_revision &&
          directory_group->member_count == report.participants_expected;
      if (matches_directory) {
        site_group_summaries->accept(*directory, report, now_ms);
      } else if (envelope.message.floorplan_id[0] != '\0' &&
                 envelope.message.site_topology_revision != 0 &&
                 configuration != nullptr) {
        const char *expected_leader = resolve_expected_group_leader(
            identity, *configuration, directory, this->endpoints_,
            site->state().leader_node_id, report.group_id, now_ms);
        site_group_summaries->accept_discovered(
            *configuration, directory, expected_leader,
            envelope.message.floorplan_id, report, now_ms);
      }
    }
    return SiteHeartbeatResult::OK;
  }
  if (envelope.message.group_id[0] == '\0')
    return SiteHeartbeatResult::OK;
  const auto *local_membership = groups->local_membership();
  if (local_membership != nullptr && local_membership->valid &&
      std::strcmp(local_membership->group_id,
                  envelope.message.group_id) != 0)
    return SiteHeartbeatResult::OK;

  GroupHeartbeat heartbeat{};
  heartbeat.participant_id = envelope.message.participant_id;
  heartbeat.group_id = envelope.message.group_id;
  heartbeat.boot_id = envelope.message.boot_id;
  heartbeat.sequence = envelope.message.sequence;
  heartbeat.term = envelope.message.group_term;
  heartbeat.lease_ms = envelope.message.lease_ms;
  heartbeat.sample_age_ms = 0;
  heartbeat.roster_revision = envelope.message.site_topology_revision;
  heartbeat.assignment_revision =
      envelope.message.group_assignment_revision;
  heartbeat.election_view_digest =
      envelope.message.group_election_view_digest;
  heartbeat.election_contract_version =
      envelope.wire_version >= HEARTBEAT_VERSION
          ? 3U
          : envelope.wire_version >= WINNER_ELECTION_HEARTBEAT_VERSION
                ? 2U
                : envelope.wire_version >= STABLE_ELECTION_HEARTBEAT_VERSION
                      ? 1U
                      : 0U;
  heartbeat.credentials_complete =
      envelope.message.site_credentials_complete;
  const auto accepted =
      groups->accept_heartbeat(participants, readiness, configuration,
                               heartbeat, now_ms);
  if (accepted != GroupHeartbeatResult::OK &&
      accepted != GroupHeartbeatResult::DUPLICATE_OR_REORDERED)
    return SiteHeartbeatResult::GROUP_REJECTED;
  return SiteHeartbeatResult::OK;
}

void SiteHeartbeatTransport::update_diagnostics_(uint32_t now_ms) {
  uint8_t endpoints = 0;
  uint8_t online = 0;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *endpoint = this->endpoints_.endpoint_at(slot);
    if (endpoint == nullptr)
      continue;
    endpoints++;
    online += ParticipantEndpointCache::is_online(endpoint, now_ms) ? 1U : 0U;
  }
  this->diagnostics_.endpoints = endpoints;
  this->diagnostics_.online_endpoints = online;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
