#include "site_heartbeat.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_heartbeat_tests: %s\n", message);
  std::exit(1);
}

SiteHeartbeatMessage message() {
  SiteHeartbeatMessage value{};
  std::strcpy(value.participant_id, "namo-node-peer");
  std::strcpy(value.site_id, "site-00112233445566778899aabbccddeeff");
  std::strcpy(value.group_id, "group-main");
  std::strcpy(value.boot_id, "boot-00112233");
  std::strcpy(value.floorplan_id, "fp-00112233445566778899aabbccddeeff");
  value.sequence = 42;
  value.site_term = 3;
  value.group_term = 7;
  value.lease_ms = 6000;
  value.site_configuration_authority_epoch = 4;
  value.site_configuration_generation = 9;
  value.site_configuration_digest = 0x1020304050607080ULL;
  value.site_topology_revision = 8;
  value.site_topology_digest = 0x8877665544332211ULL;
  value.site_directory_revision = 11;
  value.site_directory_digest = 0x13572468;
  value.site_credentials_complete = true;
  value.floorplan_geometry_version = 12;
  value.floorplan_config_hash = 0x12345678;
  value.floorplan_image_hash = 0x87654321;
  value.radar_pose_version = 17;
  value.floorplan_replica_ready = true;
  value.floorplan_owner_advertisement = true;
  value.group_summary_present = true;
  value.group_assignment_revision = 7;
  value.group_participants_expected = 7;
  value.group_participants_online = 6;
  value.site_election_view_digest = 0x10293847;
  value.group_election_view_digest = 0x56473829;
  return value;
}

void packet_round_trip_preserves_bounded_authority_fields() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 5, 0x1122334455667788ULL,
                                packet.data(), packet.size(), &signed_size,
                                &packet_size) == SiteHeartbeatCodecResult::OK,
          "a valid heartbeat should encode");
  require(signed_size + SiteHeartbeatEnvelope::TAG_SIZE == packet_size,
          "the authentication tag must be outside the signed payload");

  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
              SiteHeartbeatCodecResult::OK,
          "the encoded heartbeat should parse");
  require(decoded.key_version == 5 &&
              decoded.participant_token == 0x1122334455667788ULL &&
              decoded.message.sequence == 42 &&
              decoded.message.site_term == 3 &&
              decoded.message.group_term == 7 &&
              decoded.message.lease_ms == 6000 &&
              decoded.message.site_configuration_authority_epoch == 4 &&
              decoded.message.site_configuration_generation == 9 &&
              decoded.message.site_configuration_digest ==
                  0x1020304050607080ULL &&
              decoded.message.site_topology_revision == 8 &&
              decoded.message.site_topology_digest ==
                  0x8877665544332211ULL &&
              decoded.message.site_directory_revision == 11 &&
              decoded.message.site_directory_digest == 0x13572468 &&
              decoded.message.site_credentials_complete &&
              decoded.message.floorplan_geometry_version == 12 &&
              decoded.message.floorplan_config_hash == 0x12345678 &&
              decoded.message.floorplan_image_hash == 0x87654321 &&
              decoded.message.radar_pose_version == 17 &&
              decoded.message.floorplan_replica_ready &&
              decoded.message.floorplan_owner_advertisement &&
              decoded.message.group_summary_present &&
              decoded.message.group_assignment_revision == 7 &&
              decoded.message.group_participants_expected == 7 &&
              decoded.message.group_participants_online == 6 &&
              decoded.message.site_election_view_digest == 0x10293847 &&
              decoded.message.group_election_view_digest == 0x56473829,
          "numeric authority fields changed during round trip");
  require(std::strcmp(decoded.message.participant_id, "namo-node-peer") == 0 &&
              std::strcmp(decoded.message.site_id,
                          "site-00112233445566778899aabbccddeeff") == 0 &&
              std::strcmp(decoded.message.group_id, "group-main") == 0 &&
              std::strcmp(decoded.message.boot_id, "boot-00112233") == 0 &&
              std::strcmp(decoded.message.floorplan_id,
                          "fp-00112233445566778899aabbccddeeff") == 0,
          "identity fields changed during round trip");
}

void packet_parser_rejects_truncation_and_invalid_lengths() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::OK,
          "heartbeat fixture should encode");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size - 1, &decoded) ==
              SiteHeartbeatCodecResult::INVALID_PACKET,
          "a truncated tag must be rejected");
  packet[132] = 255;
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
              SiteHeartbeatCodecResult::INVALID_PACKET,
          "an impossible participant length must be rejected");
}

void base_heartbeat_carries_configuration_fencing_without_group_summary() {
  auto value = message();
  value.group_summary_present = false;
  value.group_assignment_revision = 0;
  value.group_participants_expected = 0;
  value.group_participants_online = 0;
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(value, 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::OK &&
              packet[4] == SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION &&
              packet[6] == SiteHeartbeatEnvelope::PREFIX_SIZE,
          "a base heartbeat must carry the current fencing contract");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              !decoded.message.group_summary_present,
          "the fenced heartbeat should parse without Group metadata");
}

void v7_compatibility_heartbeat_preserves_directory_without_credentials() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                false, true) == SiteHeartbeatCodecResult::OK &&
              packet[4] == 7 &&
              packet[6] ==
                  SiteHeartbeatEnvelope::CREDENTIAL_FENCE_PREFIX_SIZE,
          "the v7 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 7 &&
              decoded.message.site_directory_revision == 11 &&
              decoded.message.site_directory_digest == 0x13572468 &&
              !decoded.message.site_credentials_complete,
          "v7 peers must not claim successor credential readiness");
}

void v8_compatibility_heartbeat_preserves_credential_readiness_without_views() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                false, false, true) ==
                  SiteHeartbeatCodecResult::OK &&
              packet[4] == 8 &&
              packet[6] ==
                  SiteHeartbeatEnvelope::CREDENTIAL_FENCE_PREFIX_SIZE,
          "the v8 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 8 &&
              decoded.message.site_credentials_complete &&
              decoded.message.site_election_view_digest == 0 &&
              decoded.message.group_election_view_digest == 0,
          "v8 peers must retain credential readiness without claiming an election view");
}

void v9_compatibility_heartbeat_preserves_legacy_election_views() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                false, false, false, true) ==
                  SiteHeartbeatCodecResult::OK &&
              packet[4] == 9 &&
              packet[6] == SiteHeartbeatEnvelope::PREFIX_SIZE,
          "the v9 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 9 &&
              decoded.message.site_credentials_complete &&
              decoded.message.site_election_view_digest == 0x10293847 &&
              decoded.message.group_election_view_digest == 0x56473829,
          "v9 peers must retain the legacy election view contract");
}

void v10_compatibility_heartbeat_preserves_previous_election_views() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                false, false, false, false, true) ==
                  SiteHeartbeatCodecResult::OK &&
              packet[4] == 10 &&
              packet[6] == SiteHeartbeatEnvelope::PREFIX_SIZE,
          "the v10 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 10 &&
              decoded.message.site_election_view_digest == 0x10293847 &&
              decoded.message.group_election_view_digest == 0x56473829,
          "v10 peers must retain the previous election-view contract");
}

void v11_compatibility_heartbeat_preserves_winner_election_views() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                false, false, false, false, false, true) ==
                  SiteHeartbeatCodecResult::OK &&
              packet[4] == 11 &&
              packet[6] == SiteHeartbeatEnvelope::PREFIX_SIZE,
          "the v11 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 11 &&
              decoded.message.site_election_view_digest == 0x10293847 &&
              decoded.message.group_election_view_digest == 0x56473829,
          "v11 peers must retain the winner-bound election contract");
}

void quarantined_bootstrap_cannot_claim_site_authority() {
  SiteCoordinatorState inactive{};
  const auto bootstrap =
      project_site_heartbeat_authority(inactive, true);
  require(bootstrap.control_plane_only && bootstrap.site_term == 1 &&
              bootstrap.election_view_digest == 0,
          "a quarantined bootstrap must carry liveness without authority");

  auto value = message();
  value.site_term = bootstrap.site_term;
  value.site_election_view_digest = bootstrap.election_view_digest;
  value.group_summary_present = false;
  value.group_id[0] = '\0';
  value.group_term = 0;
  value.group_election_view_digest = 0;
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(
              value, 1, 1, packet.data(), packet.size(), &signed_size,
              &packet_size, false, false, false, false,
              bootstrap.control_plane_only) == SiteHeartbeatCodecResult::OK,
          "a quarantined control-plane heartbeat must encode on the v8 wire");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.wire_version == 8 && decoded.message.site_term == 1 &&
              decoded.message.site_election_view_digest == 0,
          "the bootstrap packet must not advertise an election view");

  const auto ordinary = project_site_heartbeat_authority(inactive, false);
  require(!ordinary.control_plane_only && ordinary.site_term == 0 &&
              ordinary.election_view_digest == 0,
          "an ordinary inactive Node must not inherit the bootstrap bypass");
  value.site_term = ordinary.site_term;
  require(encode_site_heartbeat(value, 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::INVALID_ARGUMENT,
          "an ordinary zero-term heartbeat must remain fail-closed");

  SiteCoordinatorState active{};
  active.term = 9;
  active.election_view_digest = 0x12345678;
  active.compatibility_v11_election_view_digest = 0x87654321;
  const auto projected_active =
      project_site_heartbeat_authority(active, false);
  require(!projected_active.control_plane_only &&
              projected_active.site_term == active.term &&
              projected_active.election_view_digest ==
                  active.election_view_digest &&
              projected_active.compatibility_v11_election_view_digest ==
                  active.compatibility_v11_election_view_digest,
          "active Site authority fields must pass through unchanged");
}

void v6_compatibility_heartbeat_preserves_site_topology_fencing() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, false,
                                true) == SiteHeartbeatCodecResult::OK &&
              packet[4] == 6 &&
              packet[6] ==
                  SiteHeartbeatEnvelope::SITE_TOPOLOGY_PREFIX_SIZE,
          "the v6 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.message.site_topology_revision == 8 &&
              decoded.message.site_topology_digest ==
                  0x8877665544332211ULL &&
              decoded.message.site_directory_revision == 0 &&
              decoded.message.site_directory_digest == 0,
          "v6 peers must receive Site topology without claiming a Directory fence");
}

void v4_compatibility_heartbeat_preserves_legacy_liveness_contract() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, true) ==
              SiteHeartbeatCodecResult::OK &&
              packet[4] == 4 &&
              packet[6] == SiteHeartbeatEnvelope::GROUP_SUMMARY_PREFIX_SIZE,
          "the compatibility heartbeat must use the established v4 wire format");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.message.site_configuration_authority_epoch == 0 &&
              decoded.message.site_configuration_generation == 9 &&
              decoded.message.site_configuration_digest == 0 &&
              decoded.message.group_summary_present,
          "v4 compatibility packets must preserve liveness without claiming a fence");
}

void v5_compatibility_heartbeat_preserves_configuration_fencing() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_heartbeat(message(), 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size, false, true) ==
              SiteHeartbeatCodecResult::OK &&
              packet[4] == 5 &&
              packet[6] ==
                  SiteHeartbeatEnvelope::CONFIGURATION_FENCE_PREFIX_SIZE,
          "the v5 compatibility heartbeat must retain its wire layout");
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), packet_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.message.site_configuration_authority_epoch == 4 &&
              decoded.message.site_configuration_digest ==
                  0x1020304050607080ULL &&
              decoded.message.site_topology_revision == 0 &&
              decoded.message.floorplan_id[0] == '\0' &&
              !decoded.message.floorplan_owner_advertisement,
          "v5 peers must receive fencing without v6 topology claims");
}

void v3_packet_parses_without_claiming_a_group_summary() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  auto value = message();
  value.floorplan_owner_advertisement = false;
  value.floorplan_id[0] = '\0';
  require(encode_site_heartbeat(value, 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::OK,
          "heartbeat fixture should encode");
  const std::size_t body_size =
      signed_size - SiteHeartbeatEnvelope::PREFIX_SIZE;
  std::memmove(packet.data() + SiteHeartbeatEnvelope::PREVIOUS_PREFIX_SIZE,
               packet.data() + SiteHeartbeatEnvelope::PREFIX_SIZE, body_size);
  packet[4] = 3;
  packet[6] = SiteHeartbeatEnvelope::PREVIOUS_PREFIX_SIZE;
  packet[57] = 0;
  packet[58] = 0;
  packet[59] = 0;
  packet[60] = static_cast<uint8_t>(std::strlen(message().participant_id));
  packet[61] = static_cast<uint8_t>(std::strlen(message().site_id));
  packet[62] = static_cast<uint8_t>(std::strlen(message().group_id));
  packet[63] = static_cast<uint8_t>(std::strlen(message().boot_id));
  const std::size_t v3_size =
      packet_size - (SiteHeartbeatEnvelope::PREFIX_SIZE -
                     SiteHeartbeatEnvelope::PREVIOUS_PREFIX_SIZE);
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), v3_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.message.floorplan_replica_ready &&
              decoded.message.radar_pose_version == 17 &&
              !decoded.message.group_summary_present,
          "v3 heartbeats must remain valid without Site Group metadata");
}

void v2_packet_parses_without_claiming_a_radar_pose() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  auto value = message();
  value.floorplan_owner_advertisement = false;
  value.floorplan_id[0] = '\0';
  require(encode_site_heartbeat(value, 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::OK,
          "heartbeat fixture should encode");
  const std::size_t body_size =
      signed_size - SiteHeartbeatEnvelope::PREFIX_SIZE;
  std::memmove(packet.data() + SiteHeartbeatEnvelope::FLOORPLAN_PREFIX_SIZE,
               packet.data() + SiteHeartbeatEnvelope::PREFIX_SIZE, body_size);
  packet[4] = 2;
  packet[6] = SiteHeartbeatEnvelope::FLOORPLAN_PREFIX_SIZE;
  packet[52] = 1;
  packet[53] = 0;
  packet[54] = 0;
  packet[55] = 0;
  packet[56] = static_cast<uint8_t>(std::strlen(message().participant_id));
  packet[57] = static_cast<uint8_t>(std::strlen(message().site_id));
  packet[58] = static_cast<uint8_t>(std::strlen(message().group_id));
  packet[59] = static_cast<uint8_t>(std::strlen(message().boot_id));
  const std::size_t v2_size =
      packet_size - (SiteHeartbeatEnvelope::PREFIX_SIZE -
                     SiteHeartbeatEnvelope::FLOORPLAN_PREFIX_SIZE);
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), v2_size, &decoded) ==
                  SiteHeartbeatCodecResult::OK &&
              decoded.message.floorplan_replica_ready &&
              decoded.message.floorplan_geometry_version == 12 &&
              decoded.message.radar_pose_version == 0,
          "v2 heartbeats must preserve replica state without inventing a pose");
}

void legacy_packet_parses_without_claiming_replica_readiness() {
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  auto value = message();
  value.floorplan_owner_advertisement = false;
  value.floorplan_id[0] = '\0';
  require(encode_site_heartbeat(value, 1, 1, packet.data(), packet.size(),
                                &signed_size, &packet_size) ==
              SiteHeartbeatCodecResult::OK,
          "heartbeat fixture should encode");
  const std::size_t body_size =
      signed_size - SiteHeartbeatEnvelope::PREFIX_SIZE;
  std::memmove(packet.data() + SiteHeartbeatEnvelope::LEGACY_PREFIX_SIZE,
               packet.data() + SiteHeartbeatEnvelope::PREFIX_SIZE, body_size);
  packet[4] = 1;
  packet[6] = SiteHeartbeatEnvelope::LEGACY_PREFIX_SIZE;
  packet[36] = static_cast<uint8_t>(std::strlen(message().participant_id));
  packet[37] = static_cast<uint8_t>(std::strlen(message().site_id));
  packet[38] = static_cast<uint8_t>(std::strlen(message().group_id));
  packet[39] = static_cast<uint8_t>(std::strlen(message().boot_id));
  const std::size_t legacy_size = packet_size -
                                  (SiteHeartbeatEnvelope::PREFIX_SIZE -
                                   SiteHeartbeatEnvelope::LEGACY_PREFIX_SIZE);
  SiteHeartbeatEnvelope decoded{};
  require(parse_site_heartbeat(packet.data(), legacy_size, &decoded) ==
              SiteHeartbeatCodecResult::OK &&
              !decoded.message.floorplan_replica_ready &&
              decoded.message.site_configuration_generation == 0,
          "legacy heartbeats must parse but cannot satisfy replica admission");
}

void authenticated_source_address_overrides_discovery_without_flash_state() {
  ParticipantEndpointCache cache{};
  ParticipantCredentialRegistry credentials{};
  std::array<uint8_t, 32> key{};
  key.fill(0x5a);
  require(credentials.set("namo-node-peer", 11, 1, key.data(), false) ==
              ParticipantCredentialResult::OK,
          "credential fixture should be accepted");
  require(cache.observe_discovery("namo-node-peer", 0x0101A8C0U, 49191,
                                  1000),
          "mDNS should seed an unverified endpoint");
  require(cache.observe_authenticated("namo-node-peer", 0x4901A8C0U, 49191,
                                      2000),
          "an authenticated packet should replace the discovered address");
  const auto *endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr && endpoint->authenticated &&
              endpoint->ipv4 == 0x4901A8C0U,
          "the authenticated source must become authoritative in RAM");
  require(ParticipantEndpointCache::is_online(endpoint, 9999) &&
              !ParticipantEndpointCache::is_online(endpoint, 10001),
          "authenticated endpoints must expire without a fresh heartbeat");
  require(!cache.observe_discovery("namo-node-peer", 0x4901A8C0U, 49191,
                                   12000) &&
              cache.find("namo-node-peer")->authenticated,
          "same-address discovery must not erase authenticated state");
  require(!cache.observe_discovery("namo-node-peer", 0x6301A8C0U, 49191,
                                   3000) &&
              cache.find("namo-node-peer")->ipv4 == 0x4901A8C0U,
          "fresh authenticated addresses must resist spoofed discovery");
  require(cache.observe_discovery("namo-node-peer", 0x6301A8C0U, 49191,
                                  18000) &&
              !cache.find("namo-node-peer")->authenticated,
          "stale addresses should be replaceable by later discovery");
  require(credentials.erase("namo-node-peer") ==
              ParticipantCredentialResult::OK,
          "credential fixture should be removable");
  cache.reconcile(credentials);
  require(cache.find("namo-node-peer") == nullptr,
          "endpoint state must disappear when its credential is removed");
}

void discovery_refresh_is_only_needed_for_endpoint_recovery() {
  ParticipantEndpointCache cache{};
  ParticipantCredentialRegistry credentials{};
  ParticipantRegistry participants{};
  std::array<uint8_t, 32> key{};
  key.fill(0x37);
  require(participants.discover("namo-node-peer", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              participants.classify("namo-node-peer",
                                    ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK &&
              credentials.set("namo-node-peer", 17, 1, key.data(), false) ==
                  ParticipantCredentialResult::OK,
          "endpoint recovery fixture should be accepted");
  require(cache.needs_discovery_refresh(credentials, participants, 1000),
          "a paired peer without an endpoint needs discovery bootstrap");
  require(cache.observe_authenticated("namo-node-peer", 0x4901A8C0U, 49191,
                                      2000),
          "an authenticated heartbeat should establish the endpoint");
  require(!cache.needs_discovery_refresh(credentials, participants, 9000),
          "a live authenticated endpoint must not trigger background search");
  require(cache.needs_discovery_refresh(credentials, participants, 10001),
          "an expired heartbeat endpoint needs discovery recovery");
  require(participants.revoke("namo-node-peer") ==
              ParticipantRegistryResult::OK &&
              !cache.needs_discovery_refresh(credentials, participants, 11000),
          "a revoked peer must not keep background discovery running");
}

void canonical_active_peer_does_not_require_a_direct_pair_record() {
  ParticipantRegistry direct_pairs{};
  require(direct_pairs.discover("namo-node-direct",
                                ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "direct pairing fixture should be accepted");

  SiteConfigurationSnapshot configuration{};
  configuration.roster_committed = true;
  auto &indirect = configuration.members[0];
  indirect.occupied = true;
  std::strcpy(indirect.participant_id, "namo-node-indirect");
  indirect.type = ParticipantType::NAMO_NODE;
  indirect.membership = ParticipantMembership::ACTIVE;
  indirect.leader_eligible = true;

  require(site_heartbeat_peer_known(direct_pairs, &configuration,
                                    "namo-node-direct"),
          "a quarantined direct peer must remain available for enrollment traffic");
  require(direct_pairs.revoke("namo-node-direct") ==
                  ParticipantRegistryResult::OK &&
              !site_heartbeat_peer_known(direct_pairs, &configuration,
                                         "namo-node-direct"),
          "an explicit direct revocation must override canonical fallback");
  require(site_heartbeat_peer_known(direct_pairs, &configuration,
                                    "namo-node-indirect"),
          "an active canonical peer must be known without a direct pair record");
  indirect.membership = ParticipantMembership::QUARANTINED;
  require(!site_heartbeat_peer_known(direct_pairs, &configuration,
                                     "namo-node-indirect"),
          "an indirect quarantined peer must not enter runtime traffic");

  indirect.membership = ParticipantMembership::ACTIVE;
  ParticipantCredentialRegistry credentials{};
  std::array<uint8_t, 32> key{};
  key.fill(0x29);
  require(credentials.set(indirect.participant_id, 29, 1, key.data(), true) ==
              ParticipantCredentialResult::OK,
          "indirect credential fixture should be accepted");
  ParticipantEndpointCache cache{};
  require(cache.needs_discovery_refresh(credentials, direct_pairs,
                                        &configuration, 1000),
          "an indirect canonical peer must request endpoint discovery");
}

void credential_transition_reopens_discovery_without_erasing_routes() {
  ParticipantEndpointCache cache{};
  require(cache.observe_authenticated("node-new-owner", 0x4E01A8C0U,
                                      49191, 2000),
          "the stale authenticated route should be established");
  cache.reset_authentication();
  const auto *endpoint = cache.find("node-new-owner");
  require(endpoint != nullptr && !endpoint->authenticated &&
              endpoint->ipv4 == 0x4E01A8C0U,
          "an Owner transition must retain a route but discard its stale authentication");
  require(cache.observe_discovery("node-new-owner", 0x6801A8C0U, 49191,
                                  2100) &&
              cache.find("node-new-owner")->ipv4 == 0x6801A8C0U,
          "mDNS must be able to replace the stale route immediately after transition");
}

void committed_directory_never_forces_site_and_group_leader_overlap() {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-home");
  std::strcpy(identity.node_id, "namo-node-site-leader");

  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, identity.site_id);
  std::strcpy(configuration.configuration_owner_node_id,
              "namo-node-owner");
  configuration.roster_committed = true;
  const char *ids[] = {identity.node_id, "namo-node-owner"};
  for (std::size_t index = 0; index < 2; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, ids[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.group_id, "group-main");
  }

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, identity.site_id);
  directory.generation = 1;
  directory.topology_revision = 1;
  directory.committed = true;
  auto &group = directory.groups[0];
  group.occupied = true;
  std::strcpy(group.group_id, "group-main");
  std::strcpy(group.floorplan_id, "floorplan-main");
  std::strcpy(group.leader_node_id, identity.node_id);
  group.assignment_revision = 9;
  group.member_count = 2;

  ParticipantEndpointCache endpoints{};
  require(endpoints.observe_authenticated("namo-node-owner", 0x4E01A8C0U,
                                          49191, 1000),
          "the preferred remote Owner should be online in the fixture");
  require(std::strcmp(resolve_expected_group_leader(
                          identity, configuration, &directory, endpoints,
                          identity.node_id, "group-main", 2000),
                      "namo-node-owner") == 0,
          "the committed Directory forced Site and Group leader overlap");
  require(std::strcmp(resolve_expected_group_leader(
                          identity, configuration, nullptr, endpoints,
                          identity.node_id, "group-main", 2000),
                      "namo-node-owner") == 0,
          "leader discovery fallback no longer follows role preference");

  std::strcpy(group.leader_node_id, "namo-node-owner");
  require(std::strcmp(resolve_expected_group_leader(
                          identity, configuration, &directory, endpoints,
                          identity.node_id, "group-main", 10001),
                      identity.node_id) == 0,
          "an expired Directory Leader blocked runtime Group handoff");
}

SiteConfigurationSnapshot expanded_group_configuration(uint8_t member_count) {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-expansion");
  std::strcpy(configuration.configuration_owner_node_id,
              "namo-node-40-owner");
  configuration.authority_epoch = 3;
  configuration.generation = member_count;
  configuration.roster_revision = member_count;
  configuration.roster_committed = true;
  const char *ids[] = {
      "namo-node-10-fallback", "namo-node-20-site",
      "namo-node-30-incumbent", "namo-node-40-owner",
      "namo-node-50",          "namo-node-60",
      "namo-node-70",
  };
  for (uint8_t index = 0; index < member_count; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, ids[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "space-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
  }
  return configuration;
}

void roster_expansion_preserves_the_valid_directory_incumbent() {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-expansion");
  std::strcpy(identity.node_id, "namo-node-20-site");

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, identity.site_id);
  directory.generation = 1;
  directory.topology_revision = 30;
  directory.committed = true;
  auto &directory_group = directory.groups[0];
  directory_group.occupied = true;
  std::strcpy(directory_group.group_id, "group-main");
  std::strcpy(directory_group.floorplan_id, "floorplan-main");
  std::strcpy(directory_group.leader_node_id,
              "namo-node-30-incumbent");
  directory_group.assignment_revision = 30;
  directory_group.member_count = 5;

  ParticipantEndpointCache endpoints{};
  const char *remote_ids[] = {
      "namo-node-10-fallback", "namo-node-30-incumbent",
      "namo-node-40-owner",    "namo-node-50",
      "namo-node-60",          "namo-node-70",
  };
  for (std::size_t index = 0; index < 6; ++index) {
    require(endpoints.observe_authenticated(
                remote_ids[index],
                static_cast<uint32_t>(0x0A000001U + index), 49191, 1000),
            "every expansion participant should have a live endpoint");
  }

  SiteGroupSummaryCache summaries{};
  auto configuration = expanded_group_configuration(6);
  const char *expected = resolve_expected_group_leader(
      identity, configuration, &directory, endpoints, identity.node_id,
      directory_group.group_id, 2000);
  require(expected != nullptr &&
              std::strcmp(expected, directory_group.leader_node_id) == 0,
          "a 5-to-6 expansion must preserve the valid Directory incumbent");

  SiteGroupSummaryReport report{};
  report.participant_id = directory_group.leader_node_id;
  report.group_id = directory_group.group_id;
  report.assignment_revision = directory_group.assignment_revision;
  report.term = 38;
  report.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  report.participants_expected = 6;
  report.participants_online = 6;
  require(summaries.accept_discovered(
              configuration, &directory, expected,
              directory_group.floorplan_id, report, 2000) ==
              SiteGroupSummaryResult::OK,
          "the incumbent summary must promote the Directory to six members");
  require(summaries.find(directory_group.group_id) != nullptr &&
              summaries.find(directory_group.group_id)->assignment_revision ==
                  31,
          "the six-member assignment must advance exactly once");

  directory_group.assignment_revision = 31;
  directory_group.member_count = 6;
  configuration = expanded_group_configuration(7);
  expected = resolve_expected_group_leader(
      identity, configuration, &directory, endpoints, identity.node_id,
      directory_group.group_id, 3000);
  report.assignment_revision = directory_group.assignment_revision;
  report.participants_expected = 7;
  report.participants_online = 7;
  require(expected != nullptr &&
              std::strcmp(expected, directory_group.leader_node_id) == 0 &&
              summaries.accept_discovered(
                  configuration, &directory, expected,
                  directory_group.floorplan_id, report, 3000) ==
                  SiteGroupSummaryResult::OK,
          "the same incumbent must carry the 6-to-7 expansion");
  require(summaries.find(directory_group.group_id) != nullptr &&
              summaries.find(directory_group.group_id)->assignment_revision ==
                  32 &&
              summaries.find(directory_group.group_id)->participants_expected ==
                  7,
          "the seven-member summary must become the next canonical assignment");

  auto ineligible_configuration = configuration;
  SiteConfigurationMember *ineligible_incumbent = nullptr;
  for (auto &member : ineligible_configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id,
                    directory_group.leader_node_id) == 0) {
      ineligible_incumbent = &member;
      break;
    }
  }
  require(ineligible_incumbent != nullptr,
          "the incumbent must exist in the expansion fixture");
  ineligible_incumbent->leader_eligible = false;
  require(std::strcmp(resolve_expected_group_leader(
                          identity, ineligible_configuration, &directory,
                          endpoints, identity.node_id,
                          directory_group.group_id, 3000),
                      "namo-node-10-fallback") == 0,
          "an ineligible incumbent must not survive roster expansion");

  directory_group.member_count = 7;
  const auto contracted_configuration = expanded_group_configuration(6);
  require(std::strcmp(resolve_expected_group_leader(
                          identity, contracted_configuration, &directory,
                          endpoints, identity.node_id,
                          directory_group.group_id, 3000),
                      "namo-node-10-fallback") == 0,
          "a roster contraction must not use the expansion continuity rule");
}

void sequence_tracking_rejects_replay_but_accepts_a_new_boot() {
  SiteHeartbeatSequenceTracker tracker{};
  require(tracker.accept(11, "boot-a", 1),
          "the first heartbeat should be accepted");
  require(!tracker.accept(11, "boot-a", 1) &&
              !tracker.accept(11, "boot-a", 0),
          "duplicates and invalid sequences must be rejected");
  require(tracker.accept(11, "boot-a", 2),
          "a newer sequence should be accepted");
  require(tracker.accept(11, "boot-b", 1),
          "a new boot ID should reset the sequence stream");
}

void endpoint_configuration_fence_detects_equal_revision_divergence() {
  PlatformIdentity local{};
  std::strcpy(local.site_id, "site-00112233445566778899aabbccddeeff");
  std::strcpy(local.node_id, "namo-node-local");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(local, false, &configuration) ==
              SiteConfigurationResult::OK,
          "endpoint fence configuration bootstrap failed");
  ParticipantEndpoint endpoint{};
  endpoint.site_configuration_authority_epoch =
      configuration.authority_epoch;
  endpoint.site_configuration_generation = configuration.generation;
  endpoint.site_configuration_digest =
      site_configuration_digest(configuration);
  require(participant_endpoint_matches_configuration(&endpoint,
                                                      configuration),
          "an identical fenced revision must match");
  endpoint.site_configuration_digest ^= 1U;
  require(!participant_endpoint_matches_configuration(&endpoint,
                                                       configuration),
          "an equal revision with a different payload must conflict");
  endpoint.site_configuration_digest =
      site_configuration_digest(configuration);
  ++endpoint.site_configuration_authority_epoch;
  require(!participant_endpoint_matches_configuration(&endpoint,
                                                       configuration),
          "a different authority epoch must not enter canonical election");
}

void bounded_roster_transition_requires_an_exact_adjacent_current_peer() {
  PlatformIdentity local{};
  std::strcpy(local.site_id, "site-00112233445566778899aabbccddeeff");
  std::strcpy(local.node_id, "namo-node-local");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(local, false, &configuration) ==
              SiteConfigurationResult::OK,
          "bounded roster transition configuration bootstrap failed");
  auto &remote = configuration.members[1];
  remote.occupied = true;
  std::strcpy(remote.participant_id, "namo-node-remote");
  remote.type = ParticipantType::NAMO_NODE;
  remote.trust = ParticipantTrust::OFFICIAL_VERIFIED;
  remote.membership = ParticipantMembership::ACTIVE;
  remote.leader_eligible = true;
  configuration.generation = 20;
  configuration.roster_revision = 12;

  ParticipantEndpoint endpoint{};
  endpoint.site_configuration_authority_epoch =
      configuration.authority_epoch;
  endpoint.site_configuration_generation = 19;
  endpoint.site_configuration_digest = 0x1111222233334444ULL;
  endpoint.site_topology_revision = 11;
  endpoint.site_topology_digest = 0x5555666677778888ULL;
  endpoint.site_credentials_complete = true;

  require(participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "the immediately previous roster revision was not admitted");
  require(participant_endpoint_matches_coordination_fence(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "the previous roster revision did not pass the coordination fence");
  endpoint.site_configuration_generation = 21;
  endpoint.site_topology_revision = 13;
  require(participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "the immediately next roster revision was not admitted");

  endpoint.site_configuration_generation = configuration.generation;
  endpoint.site_topology_revision = configuration.roster_revision;
  endpoint.site_topology_digest = site_topology_digest(configuration) ^ 1U;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "an equal revision was mislabeled as a transition");
  require(!participant_endpoint_matches_coordination_fence(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "an equal revision with a divergent digest passed coordination");
  endpoint.site_topology_digest = site_topology_digest(configuration);
  require(participant_endpoint_matches_coordination_fence(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "an exact topology failed the coordination fence");
  endpoint.site_configuration_generation = 22;
  endpoint.site_topology_revision = 14;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "a non-adjacent revision bypassed the configuration fence");
  endpoint.site_configuration_generation = 21;
  endpoint.site_topology_revision = 12;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "generation-only movement bypassed the roster fence");
  endpoint.site_topology_revision = 13;
  ++endpoint.site_configuration_authority_epoch;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "a different authority epoch bypassed the roster fence");
  --endpoint.site_configuration_authority_epoch;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION - 1U),
          "a legacy heartbeat bypassed the roster fence");
  endpoint.site_credentials_complete = false;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "an incomplete credential roster bypassed the roster fence");
  endpoint.site_credentials_complete = true;
  remote.membership = ParticipantMembership::QUARANTINED;
  require(!participant_endpoint_is_bounded_roster_transition(
              &endpoint, configuration, remote.participant_id,
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION),
          "a quarantined peer bypassed the roster fence");
}

void automatic_handover_requires_owner_outage_or_exact_endorsement() {
  constexpr uint32_t now_ms = 1000;
  constexpr uint64_t candidate_configuration_digest =
      0x1020304050607080ULL;
  constexpr uint64_t candidate_topology_digest = 0x8877665544332211ULL;
  ParticipantEndpointCache cache{};
  require(cache.observe_authenticated("namo-node-owner", 0xC0A80102U,
                                      49191, now_ms),
          "owner endpoint authentication fixture failed");
  require(cache.observe_floorplan_replica(
              "namo-node-owner", 4, 32, 0x1111222233334444ULL, 0, 0, 0,
              0, false) &&
              cache.observe_site_topology(
                  "namo-node-owner", 30, 0xAAAABBBBCCCCDDDDULL),
          "owner authority fixture failed");
  const auto *owner = cache.find("namo-node-owner");
  require(!automatic_handover_authority_available(
              owner, now_ms, 5, 33, candidate_configuration_digest, 31,
              candidate_topology_digest),
          "an online old owner endorsed a different handover revision");

  require(cache.observe_floorplan_replica(
              "namo-node-owner", 5, 33, candidate_configuration_digest, 0,
              0, 0, 0, false) &&
              cache.observe_site_topology("namo-node-owner", 31,
                                          candidate_topology_digest),
          "owner handover endorsement fixture failed");
  require(automatic_handover_authority_available(
              owner, now_ms, 5, 33, candidate_configuration_digest, 31,
              candidate_topology_digest),
          "an online owner's exact endorsement did not authorize recovery");
  require(automatic_handover_authority_available(
              owner,
              now_ms + ParticipantEndpointCache::ONLINE_TIMEOUT_MS + 1U, 6,
              34, 0x5555666677778888ULL, 32, 0x9999AAAABBBBCCCCULL),
          "an offline owner blocked a fresh exact handover");
}

void legacy_heartbeat_cannot_erase_a_known_configuration_fence() {
  ParticipantEndpointCache cache{};
  require(cache.observe_floorplan_replica(
              "namo-node-peer", 4, 9, 0x1020304050607080ULL, 12,
              0x12345678, 0x87654321, 17, true),
          "the fenced endpoint should be recorded");
  require(cache.observe_floorplan_replica(
              "namo-node-peer", 0, 9, 0, 12, 0x12345678,
              0x87654321, 17, true),
          "a compatibility heartbeat should still refresh replica metadata");
  const auto *endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr &&
              endpoint->site_configuration_authority_epoch == 4 &&
              endpoint->site_configuration_generation == 9 &&
              endpoint->site_configuration_digest ==
                  0x1020304050607080ULL,
          "a v4 compatibility packet must not erase a learned v5 fence");
  require(cache.observe_floorplan_replica(
              "namo-node-peer", 0, 10, 0, 12, 0x12345678,
              0x87654321, 17, true),
          "a changed legacy generation should still refresh replica metadata");
  endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr &&
              endpoint->site_configuration_authority_epoch == 0 &&
              endpoint->site_configuration_generation == 10 &&
              endpoint->site_configuration_digest == 0,
          "a changed v4 generation must invalidate a previously learned fence");
}

void v6_heartbeat_cannot_erase_a_known_directory_fence() {
  ParticipantEndpointCache cache{};
  require(cache.observe_site_directory("namo-node-peer", 9, 0x12345678),
          "the v7 Directory fence should be recorded");
  const auto *endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr && endpoint->site_directory_revision == 9 &&
              endpoint->site_directory_digest == 0x12345678,
          "the endpoint must retain the v7 Directory fence");
  require(!cache.observe_site_directory("namo-node-peer", 0, 0) &&
              endpoint->site_directory_revision == 9 &&
              endpoint->site_directory_digest == 0x12345678,
          "a v6 compatibility heartbeat must not erase the v7 Directory fence");
}

void v7_heartbeat_cannot_erase_known_credential_readiness() {
  ParticipantEndpointCache cache{};
  require(cache.observe_site_credentials(
              "namo-node-peer",
              SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION, true),
          "the v8 credential readiness should be recorded");
  require(cache.observe_site_credentials("namo-node-peer", 7, false),
          "the v7 compatibility heartbeat should be accepted");
  const auto *endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr &&
              endpoint->heartbeat_wire_version ==
                  SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION &&
              endpoint->site_credentials_complete,
          "a v7 compatibility packet must not erase learned readiness");
}

void v11_heartbeat_cannot_downgrade_a_known_v12_endpoint() {
  ParticipantEndpointCache cache{};
  require(cache.observe_site_credentials("namo-node-peer", 12, true),
          "the v12 capability should be recorded");
  require(cache.observe_site_credentials("namo-node-peer", 11, true),
          "the v11 compatibility heartbeat should be accepted");
  const auto *endpoint = cache.find("namo-node-peer");
  require(endpoint != nullptr && endpoint->heartbeat_wire_version == 12 &&
              endpoint->latest_heartbeat_wire_version == 11 &&
              endpoint->site_credentials_complete,
          "a v11 compatibility packet must not downgrade v12 capability");
}

void current_wire_probe_is_independent_of_peer_count() {
  require(!site_heartbeat_current_wire_probe(0),
          "an unstarted send cycle must not probe the current wire contract");

  for (std::size_t peer_count = 1; peer_count <= 6; ++peer_count) {
    std::array<bool, 6> cycle_probe_seen{};
    std::array<bool, 6> packet_probe_seen{};
    uint32_t packet_sequence = 0;

    for (uint32_t cycle = 1;
         cycle <= SITE_HEARTBEAT_CURRENT_WIRE_PROBE_INTERVAL; ++cycle) {
      const bool current_wire_probe =
          site_heartbeat_current_wire_probe(cycle);
      require(current_wire_probe ==
                  (cycle == SITE_HEARTBEAT_CURRENT_WIRE_PROBE_INTERVAL),
              "the current wire contract must be probed once per five send cycles");

      for (std::size_t peer = 0; peer < peer_count; ++peer) {
        ++packet_sequence;
        cycle_probe_seen[peer] = cycle_probe_seen[peer] || current_wire_probe;
        packet_probe_seen[peer] =
            packet_probe_seen[peer] ||
            packet_sequence % SITE_HEARTBEAT_CURRENT_WIRE_PROBE_INTERVAL == 0;
      }
    }

    for (std::size_t peer = 0; peer < peer_count; ++peer)
      require(cycle_probe_seen[peer],
              "every peer must receive a current wire probe regardless of roster size");

    if (peer_count == 5) {
      require(!packet_probe_seen[0] && !packet_probe_seen[1] &&
                  !packet_probe_seen[2] && !packet_probe_seen[3] &&
                  packet_probe_seen[4],
              "the regression fixture must reproduce five-peer packet phase lock");
    }
  }
}

}  // namespace

void run_site_heartbeat_tests() {
  packet_round_trip_preserves_bounded_authority_fields();
  packet_parser_rejects_truncation_and_invalid_lengths();
  base_heartbeat_carries_configuration_fencing_without_group_summary();
  v7_compatibility_heartbeat_preserves_directory_without_credentials();
  v8_compatibility_heartbeat_preserves_credential_readiness_without_views();
  v9_compatibility_heartbeat_preserves_legacy_election_views();
  v10_compatibility_heartbeat_preserves_previous_election_views();
  v11_compatibility_heartbeat_preserves_winner_election_views();
  quarantined_bootstrap_cannot_claim_site_authority();
  v4_compatibility_heartbeat_preserves_legacy_liveness_contract();
  v5_compatibility_heartbeat_preserves_configuration_fencing();
  v6_compatibility_heartbeat_preserves_site_topology_fencing();
  v3_packet_parses_without_claiming_a_group_summary();
  v2_packet_parses_without_claiming_a_radar_pose();
  legacy_packet_parses_without_claiming_replica_readiness();
  authenticated_source_address_overrides_discovery_without_flash_state();
  discovery_refresh_is_only_needed_for_endpoint_recovery();
  canonical_active_peer_does_not_require_a_direct_pair_record();
  credential_transition_reopens_discovery_without_erasing_routes();
  committed_directory_never_forces_site_and_group_leader_overlap();
  roster_expansion_preserves_the_valid_directory_incumbent();
  sequence_tracking_rejects_replay_but_accepts_a_new_boot();
  endpoint_configuration_fence_detects_equal_revision_divergence();
  bounded_roster_transition_requires_an_exact_adjacent_current_peer();
  automatic_handover_requires_owner_outage_or_exact_endorsement();
  legacy_heartbeat_cannot_erase_a_known_configuration_fence();
  v6_heartbeat_cannot_erase_a_known_directory_fence();
  v7_heartbeat_cannot_erase_known_credential_readiness();
  v11_heartbeat_cannot_downgrade_a_known_v12_endpoint();
  current_wire_probe_is_independent_of_peer_count();
}
