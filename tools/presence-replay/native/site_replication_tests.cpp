#include "site_replication.h"
#include "role_quorum.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_replication_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity identity(const char *site_id, const char *node_id) {
  PlatformIdentity value{};
  std::strcpy(value.site_id, site_id);
  std::strcpy(value.node_id, node_id);
  return value;
}

SiteDirectorySnapshot replication_directory(uint32_t generation = 3) {
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-home");
  directory.generation = generation;
  directory.topology_revision = 2;
  directory.committed = true;
  auto &floorplan = directory.floorplans[0];
  floorplan.occupied = true;
  floorplan.committed = true;
  std::strcpy(floorplan.floorplan_id, "fp-main");
  std::strcpy(floorplan.owner_node_id, "namo-node-owner");
  floorplan.owner_generation = 5;
  floorplan.geometry_version = 2;
  floorplan.config_hash = 100;
  floorplan.image_hash = 200;
  auto &group = directory.groups[0];
  group.occupied = true;
  std::strcpy(group.group_id, "group-main");
  std::strcpy(group.floorplan_id, "fp-main");
  std::strcpy(group.leader_node_id, "namo-node-group");
  group.assignment_revision = 2;
  group.member_count = 2;
  return directory;
}

void observe_directory_source(
    ParticipantEndpointCache *endpoints,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory, const char *participant_id,
    uint32_t now_ms = 1000) {
  require(endpoints != nullptr &&
              endpoints->observe_authenticated(participant_id, 0xC0A80102U,
                                                49191, now_ms) &&
              endpoints->observe_floorplan_replica(
                  participant_id, configuration.authority_epoch,
                  configuration.generation,
                  site_configuration_digest(configuration), 0, 0, 0, 0,
                  false) &&
              endpoints->observe_site_directory(
                  participant_id, directory.topology_revision,
                  site_directory_payload_hash(directory)),
          "directory bootstrap endpoint setup failed");
}

void provision_directory_source(ParticipantCredentialRegistry *credentials,
                                const char *participant_id) {
  std::array<uint8_t, 32> key{};
  key.fill(0x5A);
  uint64_t token = 1469598103934665603ULL;
  for (const char *cursor = participant_id; cursor != nullptr && *cursor;
       ++cursor) {
    token ^= static_cast<uint8_t>(*cursor);
    token *= 1099511628211ULL;
  }
  require(credentials != nullptr &&
              credentials->set(participant_id, token, 1, key.data()) ==
                  ParticipantCredentialResult::OK,
          "directory bootstrap credential setup failed");
}

void request_round_trip_keeps_requested_chunk_without_a_body() {
  SiteReplicationFrame frame{};
  frame.kind = SiteReplicationKind::REQUEST;
  frame.target = SiteReplicationTarget::FLOORPLAN_IMAGE;
  frame.key_version = 7;
  frame.participant_token = 0x123456789ULL;
  frame.request_id = 19;
  frame.offset = 768;
  frame.length = SiteReplicationEnvelope::MAX_CHUNK_SIZE;
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_replication_frame(
              frame, packet.data(), packet.size(), &signed_size,
              &packet_size) == SiteReplicationCodecResult::OK,
          "request encode failed");
  require(signed_size == SiteReplicationEnvelope::PREFIX_SIZE &&
              packet_size == signed_size + SiteReplicationEnvelope::TAG_SIZE,
          "request must not allocate a payload body");
  SiteReplicationEnvelope decoded{};
  require(parse_site_replication_frame(packet.data(), packet_size,
                                       &decoded) ==
              SiteReplicationCodecResult::OK &&
              decoded.frame.offset == 768 &&
              decoded.frame.length == SiteReplicationEnvelope::MAX_CHUNK_SIZE &&
              decoded.frame.body == nullptr,
          "request round trip changed range semantics");
}

void response_round_trip_is_bounded_and_rejects_corruption() {
  std::array<uint8_t, 37> body{};
  for (std::size_t index = 0; index < body.size(); ++index)
    body[index] = static_cast<uint8_t>(index + 1);
  SiteReplicationFrame frame{};
  frame.kind = SiteReplicationKind::RESPONSE;
  frame.target = SiteReplicationTarget::FLOORPLAN_CONFIG;
  frame.key_version = 1;
  frame.participant_token = 44;
  frame.request_id = 20;
  frame.offset = 384;
  frame.length = body.size();
  frame.total_size = 1024;
  frame.payload_hash = 0xAABBCCDD;
  frame.site_generation = 9;
  frame.body = body.data();
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_replication_frame(
              frame, packet.data(), packet.size(), &signed_size,
              &packet_size) == SiteReplicationCodecResult::OK,
          "response encode failed");
  SiteReplicationEnvelope decoded{};
  require(parse_site_replication_frame(packet.data(), packet_size,
                                       &decoded) ==
              SiteReplicationCodecResult::OK &&
              decoded.frame.length == body.size() &&
              std::memcmp(decoded.frame.body, body.data(), body.size()) == 0,
          "response body round trip failed");
  packet[6] = 47;
  require(parse_site_replication_frame(packet.data(), packet_size,
                                       &decoded) ==
              SiteReplicationCodecResult::INVALID_PACKET,
          "corrupt prefix must fail closed");
}

void busy_response_round_trip_is_backward_compatible_and_bodyless() {
  SiteReplicationFrame frame{};
  frame.kind = SiteReplicationKind::RESPONSE;
  frame.target = SiteReplicationTarget::SITE_CONFIGURATION;
  frame.key_version = 3;
  frame.participant_token = 77;
  frame.request_id = 24;
  frame.status = SiteReplicationWireStatus::STORAGE_FAILED;
  frame.payload_hash = SITE_REPLICATION_BUSY_PAYLOAD_HASH;
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_replication_frame(
              frame, packet.data(), packet.size(), &signed_size,
              &packet_size) == SiteReplicationCodecResult::OK,
          "busy response encode failed");
  SiteReplicationEnvelope decoded{};
  require(parse_site_replication_frame(packet.data(), packet_size,
                                       &decoded) ==
              SiteReplicationCodecResult::OK &&
              decoded.frame.status ==
                  SiteReplicationWireStatus::STORAGE_FAILED &&
              decoded.frame.payload_hash ==
                  SITE_REPLICATION_BUSY_PAYLOAD_HASH &&
              decoded.frame.length == 0 && decoded.frame.body == nullptr,
          "busy response did not round-trip through the version 1 envelope");
  require(site_replication_response_result(
              decoded.frame.status, decoded.frame.payload_hash) ==
              SiteReplicationResult::BUSY &&
              site_replication_response_result(
                  SiteReplicationWireStatus::STORAGE_FAILED, 0) ==
                  SiteReplicationResult::REMOTE_REJECTED,
          "only the authenticated reserved marker should become retryable busy");
}

void response_matching_uses_the_pending_wire_target() {
  SiteReplicationFrame frame{};
  frame.kind = SiteReplicationKind::RESPONSE;
  frame.request_id = 31;

  const std::array<SiteReplicationTarget, 5> targets{
      SiteReplicationTarget::SITE_CONFIGURATION,
      SiteReplicationTarget::FLOORPLAN_CONFIG,
      SiteReplicationTarget::FLOORPLAN_IMAGE,
      SiteReplicationTarget::SITE_DIRECTORY,
      SiteReplicationTarget::CREDENTIAL_BUNDLE,
  };
  for (const auto target : targets) {
    frame.target = target;
    require(site_replication_response_matches_request(frame, 31, target),
            "a response must match the pending wire target");
  }

  frame.target = SiteReplicationTarget::FLOORPLAN_CONFIG;
  require(!site_replication_response_matches_request(
              frame, 31, SiteReplicationTarget::SITE_CONFIGURATION),
          "the operation target must not replace the pending wire target");
  require(!site_replication_response_matches_request(
              frame, 32, SiteReplicationTarget::FLOORPLAN_CONFIG),
          "a response with a stale request id must be rejected");
  frame.kind = SiteReplicationKind::REQUEST;
  require(!site_replication_response_matches_request(
              frame, 31, SiteReplicationTarget::FLOORPLAN_CONFIG),
          "a request frame must not be admitted as a response");
}

void directory_target_uses_the_existing_bounded_envelope() {
  SiteReplicationFrame frame{};
  frame.kind = SiteReplicationKind::REQUEST;
  frame.target = SiteReplicationTarget::SITE_DIRECTORY;
  frame.key_version = 2;
  frame.participant_token = 9;
  frame.request_id = 21;
  frame.offset = 384;
  frame.length = SiteReplicationEnvelope::MAX_CHUNK_SIZE;
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_site_replication_frame(
              frame, packet.data(), packet.size(), &signed_size,
              &packet_size) == SiteReplicationCodecResult::OK,
          "directory request did not reuse the replication envelope");
  SiteReplicationEnvelope decoded{};
  require(parse_site_replication_frame(packet.data(), packet_size,
                                       &decoded) ==
              SiteReplicationCodecResult::OK &&
              decoded.frame.target ==
                  SiteReplicationTarget::SITE_DIRECTORY &&
              decoded.frame.offset == 384 &&
              decoded.frame.length ==
                  SiteReplicationEnvelope::MAX_CHUNK_SIZE,
          "directory target did not round-trip");
}

void floorplan_manifest_commit_is_owner_only() {
  const auto local = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "owner bootstrap failed");
  SiteConfigurationSnapshot updated{};
  require(commit_local_floorplan_manifest(
              local, current, "fp-1234", 8, 100, 200, &updated) ==
              SiteConfigurationResult::OK &&
              updated.floorplan.committed &&
              updated.floorplan.geometry_version == 8 &&
              updated.floorplan.config_hash == 100 &&
              updated.floorplan.image_hash == 200,
          "owner floorplan manifest commit failed");
  require(commit_local_floorplan_manifest(
              identity("site-home", "namo-node-peer"), current, "fp-1234",
              8, 100, 200, &updated) == SiteConfigurationResult::NOT_OWNER,
          "non-owner committed a floorplan manifest");
}

void replication_validation_detects_stale_and_split_revisions() {
  const auto local = identity("site-home", "namo-node-local");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(local, false, &current) ==
              SiteConfigurationResult::OK,
          "validation bootstrap failed");
  current.generation = 2;
  SiteConfigurationSnapshot remote = current;
  require(validate_replicated_site_configuration(local, current, remote) ==
              SiteConfigurationReplicationResult::ALREADY_CURRENT,
          "equal canonical configuration should be current");
  std::strcpy(remote.floorplan.floorplan_id, "fp-conflict");
  remote.floorplan.committed = true;
  remote.floorplan.geometry_version = 1;
  remote.floorplan.config_hash = 3;
  remote.floorplan.image_hash = 4;
  require(validate_replicated_site_configuration(local, current, remote) ==
              SiteConfigurationReplicationResult::REVISION_CONFLICT,
          "same generation with different content must conflict");
  remote = current;
  remote.generation = current.generation - 1;
  require(validate_replicated_site_configuration(local, current, remote) ==
              SiteConfigurationReplicationResult::STALE_REMOTE,
          "older remote generation must be rejected");
}

void activation_requires_an_authenticated_matching_floorplan_replica() {
  const auto owner = identity("site-home", "namo-node-owner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK,
          "activation bootstrap failed");
  ParticipantRegistry participants{};
  require(participants.discover("namo-node-peer", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              participants.classify("namo-node-peer",
                                    ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK,
          "activation participant fixture failed");
  SiteConfigurationSnapshot roster{};
  require(commit_local_site_roster(owner, participants, configuration,
                                   &roster) == SiteConfigurationResult::OK,
          "activation roster commit failed");
  SiteConfigurationSnapshot committed{};
  require(commit_local_floorplan_manifest(owner, roster, "fp-main", 4, 100,
                                          200, &committed) ==
              SiteConfigurationResult::OK,
          "activation floorplan commit failed");
  ParticipantReadinessRegistry readiness_registry{};
  ParticipantReadinessConfig readiness{};
  readiness.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                               PARTICIPANT_CAP_PRESENCE;
  readiness.max_targets = 3;
  std::strcpy(readiness.floorplan_id, "fp-main");
  std::strcpy(readiness.space_id, "room-main");
  std::strcpy(readiness.group_id, "group-main");
  readiness.geometry_version = 4;
  readiness.local_pose_version = 1;
  readiness.pose_version = 1;
  readiness.calibration_version = 1;
  readiness.transform.residual_mm = 0;
  readiness.transform.sample_count = 1;
  readiness.transform.valid = true;
  require(readiness_registry.configure("namo-node-peer", readiness) ==
              ParticipantReadinessResult::OK,
          "activation readiness fixture failed");
  const auto *record = readiness_registry.find("namo-node-peer");
  require(validate_namo_node_activation(
              committed, record, "namo-node-peer", true, true,
              committed.generation, committed.floorplan.geometry_version,
              committed.floorplan.config_hash,
              committed.floorplan.image_hash) ==
              SiteActivationAdmissionResult::OK,
          "matching authenticated replica should pass admission");
  require(validate_namo_node_activation(
              committed, record, "namo-node-peer", true, true,
              committed.generation, committed.floorplan.geometry_version,
              committed.floorplan.config_hash + 1,
              committed.floorplan.image_hash) ==
              SiteActivationAdmissionResult::REVISION_MISMATCH,
          "a mismatched payload hash must fail admission");
  require(validate_namo_node_activation(
              committed, record, "namo-node-peer", false, true,
              committed.generation, committed.floorplan.geometry_version,
              committed.floorplan.config_hash,
              committed.floorplan.image_hash) ==
              SiteActivationAdmissionResult::ENDPOINT_OFFLINE,
          "a stale heartbeat must fail admission");

  SiteConfigurationSnapshot full_group = committed;
  std::size_t active_group_members = 0;
  for (auto &member : full_group.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, "namo-node-peer") == 0)
      continue;
    member.membership = ParticipantMembership::ACTIVE;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
    ++active_group_members;
  }
  for (std::size_t slot = 2; active_group_members < 7; ++slot) {
    auto &member = full_group.members[slot];
    member = {};
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-active-%u",
                  static_cast<unsigned>(active_group_members));
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
    ++active_group_members;
  }
  require(validate_namo_node_activation(
              full_group, record, "namo-node-peer", true, true,
              full_group.generation,
              full_group.floorplan.geometry_version,
              full_group.floorplan.config_hash,
              full_group.floorplan.image_hash) ==
              SiteActivationAdmissionResult::GROUP_FULL,
          "an eighth sensing participant must remain quarantined");
}

void automatic_replication_uses_stable_roster_slots_and_bounded_backoff() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-owner");
  configuration.members[0].occupied = true;
  std::strcpy(configuration.members[0].participant_id, "namo-node-a");
  configuration.members[1].occupied = true;
  std::strcpy(configuration.members[1].participant_id, "namo-node-b");
  configuration.members[2].occupied = true;
  std::strcpy(configuration.members[2].participant_id, "namo-node-owner");

  require(site_replication_slot_delay_ms(configuration, "namo-node-a") ==
              2500 &&
              site_replication_slot_delay_ms(configuration, "namo-node-b") ==
                  5000,
          "canonical followers must receive deterministic 2.5 second slots");
  require(site_replication_slot_delay_ms(configuration, "namo-node-missing") ==
              std::numeric_limits<uint32_t>::max(),
          "a node outside the canonical roster must not auto-sync");
  require(site_replication_source_slot_delay_ms(
              configuration, "namo-node-a", "namo-node-b") == 2500 &&
              site_replication_source_slot_delay_ms(
                  configuration, "namo-node-a", "namo-node-owner") == 5000,
          "handover replication must stagger followers from the new source");
  require(site_replication_retry_delay_ms(1) == 2500 &&
              site_replication_retry_delay_ms(2) == 5000 &&
              site_replication_retry_delay_ms(3) == 10000 &&
              site_replication_retry_delay_ms(5) == 30000 &&
              site_replication_retry_delay_ms(20) == 30000,
          "whole-session retry backoff must remain bounded");
  require(SITE_REPLICATION_BUSY_RETRY_DELAY_MS == 2750 &&
              site_replication_busy_retry_allowed(0) &&
              site_replication_busy_retry_allowed(5) &&
              !site_replication_busy_retry_allowed(6) &&
              SITE_REPLICATION_BUSY_RETRY_DELAY_MS *
                      SITE_REPLICATION_MAX_BUSY_RETRIES >=
                  SiteReplicationSourceLease::LEASE_MS * 6U,
          "source-busy retry window must cover all bounded follower slots");
}

void floorplan_source_lease_serializes_without_a_waiting_queue() {
  SiteReplicationSourceLease lease{};
  const uint32_t first_ip = 0x01020304;
  const uint32_t second_ip = 0x05060708;
  require(lease.admit("namo-node-a", first_ip, 100) ==
              SiteReplicationSourceAdmission::ACQUIRED,
          "the first authenticated follower should acquire the source");
  require(lease.admit("namo-node-a", first_ip, 500) ==
              SiteReplicationSourceAdmission::CONTINUED &&
              lease.remaining_ms(500) ==
                  SiteReplicationSourceLease::LEASE_MS,
          "the active follower should refresh its lease");
  for (uint8_t follower = 0; follower < 6; ++follower) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id),
                  "namo-node-waiting-%u",
                  static_cast<unsigned>(follower));
    require(lease.admit(participant_id, second_ip + follower, 600) ==
                SiteReplicationSourceAdmission::BUSY,
            "a second follower must receive busy instead of being queued");
  }
  require(lease.diagnostics().accepted == 1 &&
              lease.diagnostics().busy == 6 &&
              std::strcmp(lease.diagnostics().participant_id,
                          "namo-node-a") == 0,
          "busy followers must not replace the active source lease");
  require(!lease.release("namo-node-waiting-0", second_ip, true) &&
              lease.release("namo-node-a", first_ip, true) &&
              !lease.diagnostics().active &&
              lease.diagnostics().completed == 1,
          "only the active follower may complete the source lease");

  require(lease.admit("namo-node-b", second_ip, 1000) ==
              SiteReplicationSourceAdmission::ACQUIRED &&
              lease.expire(1000 + SiteReplicationSourceLease::LEASE_MS) &&
              !lease.diagnostics().active &&
              lease.diagnostics().expired == 1,
          "an abandoned source lease must expire without retaining a queue");
  require(lease.admit("namo-node-c", second_ip + 1,
                      1000 + SiteReplicationSourceLease::LEASE_MS) ==
              SiteReplicationSourceAdmission::ACQUIRED &&
              lease.diagnostics().accepted == 3,
          "the next follower should acquire the source after expiry");
}

void repeated_replication_loss_stays_bounded_and_releases_the_source() {
  uint32_t previous_delay = 0;
  for (uint8_t retry = 1; retry <= 50; ++retry) {
    const uint32_t delay = site_replication_retry_delay_ms(retry);
    require(delay >= previous_delay && delay <= 30000,
            "packet-loss retry backoff must be monotonic and capped");
    previous_delay = delay;
  }

  SiteReplicationSourceLease lease{};
  uint32_t now_ms = 100;
  for (uint8_t attempt = 0; attempt < 7; ++attempt) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id),
                  "namo-node-loss-%u", static_cast<unsigned>(attempt));
    require(lease.admit(participant_id, 0x0A000001U + attempt, now_ms) ==
                SiteReplicationSourceAdmission::ACQUIRED,
            "a retrying follower could not reacquire the replication source");
    now_ms += SiteReplicationSourceLease::LEASE_MS;
    require(lease.expire(now_ms),
            "a response-loss session retained the source lease forever");
  }
  require(!lease.diagnostics().active &&
              lease.diagnostics().accepted == 7 &&
              lease.diagnostics().expired == 7,
          "repeated response loss leaked or skipped a source lease");
}

void replication_replay_cache_is_idempotent_and_bounded() {
  SiteReplicationReplayCache cache{};
  SiteReplicationFrame request{};
  request.kind = SiteReplicationKind::REQUEST;
  request.target = SiteReplicationTarget::FLOORPLAN_CONFIG;
  request.request_id = 40;
  request.offset = 384;
  request.length = 384;
  const std::array<uint8_t, 5> response{{1, 2, 3, 4, 5}};

  require(cache.classify(10, 0x01020304, request, 100) ==
              SiteReplicationReplayAdmission::NEW_REQUEST,
          "the first authenticated request must be admitted");
  cache.remember(10, 0x01020304, request, response.data(), response.size(),
                 100);
  require(cache.classify(10, 0x01020304, request, 200) ==
              SiteReplicationReplayAdmission::DUPLICATE &&
              cache.packet_size() == response.size() &&
              std::memcmp(cache.packet(), response.data(), response.size()) ==
                  0,
          "an exact retry must reuse the cached signed response");

  auto stale = request;
  stale.offset = 768;
  require(cache.classify(10, 0x01020304, stale, 300) ==
              SiteReplicationReplayAdmission::STALE,
          "a reused request id with different content must fail closed");
  stale = request;
  stale.request_id = 39;
  require(cache.classify(10, 0x01020304, stale, 300) ==
              SiteReplicationReplayAdmission::STALE,
          "an older request id must not repeat storage work");

  auto next = request;
  next.request_id = 41;
  require(cache.classify(10, 0x01020304, next, 300) ==
              SiteReplicationReplayAdmission::NEW_REQUEST &&
              cache.classify(11, 0x01020305, request, 300) ==
                  SiteReplicationReplayAdmission::NEW_REQUEST,
          "a newer request or a different authenticated source must proceed");
  SiteReplicationReplayCache wide_token_cache{};
  wide_token_cache.remember(0x100000001ULL, 0x01020304, request,
                            response.data(), response.size(), 100);
  require(wide_token_cache.classify(1, 0x01020304, request, 200) ==
              SiteReplicationReplayAdmission::NEW_REQUEST,
          "the replay key must retain all 64 participant-token bits");
  require(cache.classify(10, 0x01020304, request,
                         100 + SiteReplicationSourceLease::LEASE_MS) ==
              SiteReplicationReplayAdmission::NEW_REQUEST,
          "the replay fence must expire instead of retaining peers forever");
}

void canonical_directory_routes_only_to_the_assigned_floorplan_owner() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-legacy");
  configuration.generation = 8;
  configuration.roster_revision = 3;
  configuration.roster_committed = true;
  configuration.members[0].occupied = true;
  std::strcpy(configuration.members[0].participant_id, "namo-node-local");
  configuration.members[0].type = ParticipantType::NAMO_NODE;
  configuration.members[0].membership = ParticipantMembership::ACTIVE;
  configuration.members[0].leader_eligible = true;
  configuration.members[0].spatial_profile.committed = true;
  std::strcpy(configuration.members[0].spatial_profile.group_id,
              "group-office");

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-home");
  directory.generation = 4;
  directory.topology_revision = 3;
  directory.committed = true;
  auto &floorplan = directory.floorplans[0];
  floorplan.occupied = true;
  floorplan.committed = true;
  std::strcpy(floorplan.floorplan_id, "fp-office");
  std::strcpy(floorplan.owner_node_id, "namo-node-floorplan-owner");
  floorplan.owner_generation = 11;
  floorplan.geometry_version = 7;
  floorplan.config_hash = 101;
  floorplan.image_hash = 202;
  auto &group = directory.groups[0];
  group.occupied = true;
  std::strcpy(group.group_id, "group-office");
  std::strcpy(group.floorplan_id, "fp-office");
  std::strcpy(group.leader_node_id, "namo-node-local");
  group.assignment_revision = 2;
  group.member_count = 2;
  require(valid_site_directory(directory),
          "canonical replication directory fixture is invalid");

  SiteReplicationRoute route{};
  require(resolve_site_replication_route(
              &directory, configuration, "namo-node-local", nullptr,
              &route) == SiteReplicationRouteResult::OK &&
              route.manifest_bound &&
              std::strcmp(route.floorplan.owner_node_id,
                          "namo-node-floorplan-owner") == 0 &&
              std::strcmp(route.floorplan.floorplan_id, "fp-office") == 0,
          "canonical directory did not resolve the assigned Floorplan owner");
  SiteConfigurationSnapshot remote = configuration;
  std::strcpy(remote.configuration_owner_node_id,
              "namo-node-floorplan-owner");
  remote.generation = 11;
  remote.floorplan.committed = true;
  std::strcpy(remote.floorplan.floorplan_id, "fp-office");
  remote.floorplan.geometry_version = 7;
  remote.floorplan.config_hash = 101;
  remote.floorplan.image_hash = 202;
  require(site_replication_route_matches_configuration(route, remote),
          "the assigned Floorplan manifest should pass the route gate");
  const auto local = identity("site-home", "namo-node-local");
  require(validate_replicated_floorplan_configuration(
              local, configuration, remote, route) ==
              SiteConfigurationReplicationResult::OK,
          "an independently versioned assigned Floorplan should replicate");
  const auto converged = remote;
  require(validate_replicated_floorplan_configuration(
              local, converged, remote, route) ==
              SiteConfigurationReplicationResult::ALREADY_CURRENT,
          "an already converged Floorplan should not replicate again");
  remote.floorplan.image_hash = 203;
  require(!site_replication_route_matches_configuration(route, remote),
          "a different Floorplan payload must fail the route gate");
  require(validate_replicated_floorplan_configuration(
              local, configuration, remote, route) ==
              SiteConfigurationReplicationResult::OWNER_MISMATCH,
          "a payload outside the assigned manifest must fail closed");
  require(resolve_site_replication_route(
              &directory, configuration, "namo-node-local",
              "namo-node-wrong-owner",
              &route) == SiteReplicationRouteResult::OWNER_MISMATCH,
          "an explicit wrong owner must not override the canonical directory");
}

void replication_route_preserves_legacy_compatibility_and_fails_closed() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id,
              "namo-node-legacy-owner");
  configuration.generation = 3;
  configuration.members[0].occupied = true;
  std::strcpy(configuration.members[0].participant_id, "namo-node-local");

  SiteDirectorySnapshot mirror{};
  mirror.compatibility_mirror = true;
  SiteReplicationRoute route{};
  require(resolve_site_replication_route(
              &mirror, configuration, "namo-node-local", nullptr,
              &route) == SiteReplicationRouteResult::LEGACY_FALLBACK &&
              std::strcmp(route.floorplan.owner_node_id,
                          "namo-node-legacy-owner") == 0,
          "an omitted legacy owner must use the persisted owner");
  require(resolve_site_replication_route(
              &mirror, configuration, "namo-node-local",
              "namo-node-explicit-owner",
              &route) == SiteReplicationRouteResult::LEGACY_FALLBACK &&
              !route.manifest_bound &&
              std::strcmp(route.floorplan.owner_node_id,
                          "namo-node-explicit-owner") == 0,
          "the compatibility mirror must preserve legacy explicit sync");

  SiteDirectorySnapshot canonical{};
  std::strcpy(canonical.site_id, "site-home");
  canonical.generation = 1;
  canonical.committed = true;
  require(resolve_site_replication_route(
              &canonical, configuration, "namo-node-local", nullptr,
              &route) == SiteReplicationRouteResult::ASSIGNMENT_MISSING,
          "a canonical directory without a local Group assignment must fail");
}

void directory_replication_uses_topology_revision_not_local_generation() {
  auto current = replication_directory(3);
  auto remote = current;
  remote.generation = 41;
  require(validate_replicated_site_directory("site-home", current, remote) ==
              SiteDirectoryReplicationValidation::ALREADY_CURRENT,
          "local persistence generation must not affect directory equality");
  require(site_directory_payload_hash(current) ==
              site_directory_payload_hash(remote),
          "the semantic directory hash must ignore local persistence generation");

  remote.groups[0].member_count = 3;
  require(validate_replicated_site_directory("site-home", current, remote) ==
              SiteDirectoryReplicationValidation::REVISION_CONFLICT,
          "equal topology directory divergence must fail closed");
  require(validate_replicated_site_directory("site-home", current, remote,
                                             true) ==
              SiteDirectoryReplicationValidation::OK,
          "the elected Site Leader must repair equal-revision split brain");

  auto compatibility = current;
  compatibility.compatibility_mirror = true;
  require(validate_replicated_site_directory("site-home", compatibility,
                                             remote) ==
              SiteDirectoryReplicationValidation::OK,
          "an equal-topology canonical directory must replace a compatibility mirror");
  compatibility.topology_revision = current.topology_revision + 100;
  remote = current;
  require(validate_replicated_site_directory("site-home", compatibility,
                                             remote) ==
              SiteDirectoryReplicationValidation::OK,
          "a compatibility mirror revision must not fence a canonical Directory revision from its independent sequence");

  remote = current;
  remote.generation = 99;
  remote.topology_revision--;
  require(validate_replicated_site_directory("site-home", current, remote) ==
              SiteDirectoryReplicationValidation::STALE_REMOTE,
          "stale Site directory must be rejected");
  require(validate_replicated_site_directory("site-home", current, remote,
                                             true) ==
              SiteDirectoryReplicationValidation::OK,
          "the elected Site Leader must replace a newer derived follower cache");

  remote = current;
  remote.generation = 1;
  ++remote.topology_revision;
  require(validate_replicated_site_directory("site-home", current, remote) ==
              SiteDirectoryReplicationValidation::OK,
          "a newer valid Site directory should be admitted");

  std::strcpy(remote.site_id, "site-other");
  require(validate_replicated_site_directory("site-home", current, remote) ==
              SiteDirectoryReplicationValidation::INVALID_DIRECTORY,
          "a foreign Site directory must be rejected");
}

void only_the_site_leader_serves_directory_to_active_namo_members() {
  const auto local = identity("site-home", "namo-node-site");
  const auto directory = replication_directory();
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  configuration.roster_committed = true;
  auto &participant = configuration.members[0];
  participant.occupied = true;
  std::strcpy(participant.participant_id, "namo-node-group");
  participant.type = ParticipantType::NAMO_NODE;
  participant.membership = ParticipantMembership::ACTIVE;
  participant.leader_eligible = true;
  SiteCoordinatorState site{};
  std::strcpy(site.leader_node_id, local.node_id);
  site.local_leader = true;
  require(site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "an eligible successor should receive Site metadata");

  std::strcpy(participant.participant_id, "namo-node-member");
  require(site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "every eligible successor must retain current Site metadata");
  participant.leader_eligible = false;
  require(site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "an active non-candidate member must retain the Site directory");
  participant.membership = ParticipantMembership::REVOKED;
  require(!site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "a revoked member must not pull the Site directory");
  participant.membership = ParticipantMembership::ACTIVE;
  participant.type = ParticipantType::SATELLITE;
  require(!site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "a satellite must not pull the NAMO Site directory");
  std::strcpy(participant.participant_id, "namo-node-group");
  participant.type = ParticipantType::NAMO_NODE;
  participant.leader_eligible = true;
  site.local_leader = false;
  require(!site_directory_request_authorized(
              local, configuration, participant.participant_id, directory,
              site),
          "a non-Site-Leader must not serve canonical directory state");
}

void authenticated_indirect_site_members_are_admitted() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  configuration.roster_committed = true;
  for (uint8_t index = 0; index < 7; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-%02u", static_cast<unsigned>(index));
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
  }

  ParticipantRegistry participants{};
  for (uint8_t index = 0; index < 7; ++index) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    require(site_replication_participant_authorized(
                participants, configuration, participant_id),
            "an authenticated canonical member required a direct registry edge");
  }
  require(!site_replication_participant_authorized(
              participants, configuration, "namo-node-unknown"),
          "an unknown authenticated identity was admitted");

  require(participants.discover("namo-node-03",
                                ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              participants.classify("namo-node-03",
                                    ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK &&
              participants.revoke("namo-node-03") ==
                  ParticipantRegistryResult::OK,
          "revoked participant fixture failed");
  require(!site_replication_participant_authorized(
              participants, configuration, "namo-node-03"),
          "an explicitly revoked direct participant was re-admitted");

  ParticipantRegistry pairing_participants{};
  require(pairing_participants.discover("namo-node-pairing",
                                        ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              site_replication_participant_authorized(
                  pairing_participants, configuration,
                  "namo-node-pairing"),
          "an authenticated quarantined pairing peer was blocked");
}

void quorum_authority_evidence_allows_read_only_replica_sources() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-04");
  configuration.authority_epoch = 7;
  configuration.generation = 31;
  configuration.roster_revision = 12;
  configuration.roster_committed = true;
  for (uint8_t index = 0; index < 5; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-%02u", static_cast<unsigned>(index));
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
  }

  const auto replica = identity("site-home", "namo-node-02");
  require(site_configuration_replica_request_authorized(
              replica, configuration, "namo-node-00") &&
              !site_configuration_replica_request_authorized(
                  replica, configuration, "namo-node-missing"),
          "an active replica must serve only active canonical members");
  configuration.members[0].membership = ParticipantMembership::QUARANTINED;
  require(site_configuration_replica_request_authorized(
              replica, configuration, "namo-node-00"),
          "an authenticated quarantined canonical member must be able to read the Site Configuration needed to rejoin");
  configuration.members[0].membership = ParticipantMembership::REVOKED;
  require(!site_configuration_replica_request_authorized(
              replica, configuration, "namo-node-00"),
          "a revoked canonical member must not read Site Configuration");
  configuration.members[0].membership = ParticipantMembership::ACTIVE;
  configuration.members[2].membership = ParticipantMembership::REVOKED;
  require(!site_configuration_replica_request_authorized(
              replica, configuration, "namo-node-00"),
          "a revoked local replica must not serve authority state");
  configuration.members[2].membership = ParticipantMembership::ACTIVE;

  ParticipantEndpointCache endpoints{};
  const uint64_t digest = site_configuration_digest(configuration);
  const uint64_t topology_digest = site_topology_digest(configuration);
  for (uint8_t index = 1; index <= 3; ++index) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    require(endpoints.observe_authenticated(
                participant_id, 0xC0A80100U + index, 49191, 1000) &&
                endpoints.observe_floorplan_replica(
                    participant_id, configuration.authority_epoch,
                    configuration.generation, digest, 0, 0, 0, 0, false) &&
                endpoints.observe_site_topology(
                    participant_id, configuration.roster_revision,
                    topology_digest) &&
                endpoints.observe_site_credentials(
                    participant_id,
                    SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION, true),
            "quorum evidence endpoint setup failed");
  }
  SiteConfigurationSnapshot stale = configuration;
  stale.authority_epoch--;
  stale.generation--;
  stale.roster_revision--;
  SiteAuthorityReplicationEvidence evidence{};
  evidence.authority_epoch = configuration.authority_epoch;
  evidence.generation = configuration.generation;
  evidence.configuration_digest = digest;
  evidence.topology_revision = configuration.roster_revision;
  evidence.topology_digest = topology_digest;
  evidence.endorsements = 3;
  evidence.quorum_required = 3;
  require(site_authority_rejoin_evidence_matches(stale, endpoints,
                                                   evidence, 1000),
          "matching three-of-five authority evidence was rejected");
  evidence.configuration_digest ^= 1U;
  require(!site_authority_rejoin_evidence_matches(stale, endpoints,
                                                    evidence, 1000),
          "mismatched authority evidence must fail closed");
  evidence.configuration_digest = digest;
  require(!site_authority_rejoin_evidence_matches(
              stale, endpoints, evidence,
              1000 + ParticipantEndpointCache::ONLINE_TIMEOUT_MS + 1),
          "expired endorsements must not authorize authority rejoin");
}

void every_active_namo_member_pulls_the_directory() {
  SiteCoordinatorState site{};
  std::strcpy(site.leader_node_id, "namo-node-site");
  site.local_leader = false;
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-site");
  configuration.roster_committed = true;
  auto &member = configuration.members[0];
  member.occupied = true;
  std::strcpy(member.participant_id, "namo-node-member");
  member.type = ParticipantType::NAMO_NODE;
  member.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  member.membership = ParticipantMembership::ACTIVE;
  member.leader_eligible = false;
  require(site_directory_replication_required(
              site, configuration, member.participant_id),
          "an active non-candidate member should pull the Site directory");

  site.local_leader = true;
  require(!site_directory_replication_required(
              site, configuration, member.participant_id),
          "the Site Leader must not pull its own directory");
  site.local_leader = false;
  member.membership = ParticipantMembership::REVOKED;
  require(!site_directory_replication_required(
              site, configuration, member.participant_id),
          "a revoked Node must not pull the Site directory");
  member.membership = ParticipantMembership::ACTIVE;
  member.type = ParticipantType::SATELLITE;
  require(!site_directory_replication_required(
              site, configuration, member.participant_id),
          "a satellite must not pull the NAMO Site directory");
  member.type = ParticipantType::NAMO_NODE;
  site.leader_node_id[0] = '\0';
  require(!site_directory_replication_required(
              site, configuration, member.participant_id),
          "directory replication must wait until Site authority exists");
}

void mixed_fallback_policy_replicates_directory_to_seven_nodes() {
  const auto leader = identity("site-home", "namo-node-00");
  const auto directory = replication_directory();
  SiteCoordinatorState site{};
  std::strcpy(site.leader_node_id, leader.node_id);
  site.local_leader = false;
  SiteCoordinatorState serving_site{};
  std::strcpy(serving_site.leader_node_id, leader.node_id);
  serving_site.local_leader = true;

  for (uint8_t count = 2; count <= 7; ++count) {
    SiteConfigurationSnapshot configuration{};
    std::strcpy(configuration.site_id, leader.site_id);
    std::strcpy(configuration.configuration_owner_node_id, leader.node_id);
    configuration.roster_committed = true;
    for (uint8_t index = 0; index < count; ++index) {
      auto &member = configuration.members[index];
      member.occupied = true;
      std::snprintf(member.participant_id, sizeof(member.participant_id),
                    "namo-node-%02u", static_cast<unsigned>(index));
      member.type = ParticipantType::NAMO_NODE;
      member.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
      member.membership = ParticipantMembership::ACTIVE;
      member.leader_eligible = index == 0 || (index % 2U) == 0;
    }
    for (uint8_t index = 1; index < count; ++index) {
      const auto &member = configuration.members[index];
      require(site_directory_replication_required(
                  site, configuration, member.participant_id) &&
                  site_directory_request_authorized(
                      leader, configuration, member.participant_id,
                      directory, serving_site),
              "every active member must retain Site metadata regardless of fallback policy");
    }
  }
}

void initial_directory_bootstrap_is_bounded_and_preserves_the_quorum_fence() {
  const auto canonical = replication_directory();
  for (uint8_t count = 2; count <= 7; ++count) {
    SiteConfigurationSnapshot configuration{};
    std::strcpy(configuration.site_id, "site-home");
    std::strcpy(configuration.configuration_owner_node_id, "namo-node-00");
    configuration.authority_epoch = 2;
    configuration.generation = 8;
    configuration.roster_revision = 6;
    configuration.roster_committed = true;
    for (uint8_t index = 0; index < count; ++index) {
      auto &member = configuration.members[index];
      member.occupied = true;
      std::snprintf(member.participant_id, sizeof(member.participant_id),
                    "namo-node-%02u", static_cast<unsigned>(index));
      member.type = ParticipantType::NAMO_NODE;
      member.membership = index + 1U == count
                              ? ParticipantMembership::QUARANTINED
                              : ParticipantMembership::ACTIVE;
      member.leader_eligible = true;
    }

    char local_id[32]{};
    std::snprintf(local_id, sizeof(local_id), "namo-node-%02u",
                  static_cast<unsigned>(count - 1U));
    const auto local = identity("site-home", local_id);
    SiteDirectorySnapshot compatibility = canonical;
    compatibility.committed = false;
    compatibility.compatibility_mirror = true;
    require(!site_directory_is_canonical_for_configuration(
                configuration, compatibility),
            "an enrollment mirror must never be admitted as canonical coordination state");
    SiteCoordinatorState site{};
    std::strcpy(site.leader_node_id, "namo-node-00");

    ParticipantEndpointCache endpoints{};
    ParticipantCredentialRegistry credentials{};
    observe_directory_source(&endpoints, configuration, canonical,
                             "namo-node-00");
    provision_directory_source(&credentials, "namo-node-00");
    const char *source = site_directory_bootstrap_source(
        local, configuration, compatibility, site, endpoints, credentials,
        1000);
    require(source != nullptr && std::strcmp(source, "namo-node-00") == 0,
            "a quarantined node must bootstrap from the elected Site Leader at every supported Site size");

    compatibility.committed = true;
    compatibility.compatibility_mirror = false;
    require(site_directory_is_canonical_for_configuration(
                configuration, compatibility),
            "a committed replicated Directory must become canonical before activation");
    std::strcpy(compatibility.groups[0].leader_node_id,
                "namo-node-conflict");
    require(site_directory_bootstrap_source(
                local, configuration, compatibility, site, endpoints,
                credentials, 1000) == nullptr,
            "a nonzero canonical Directory must never enter the initial bootstrap bypass");
  }

  SiteConfigurationSnapshot pair{};
  std::strcpy(pair.site_id, "site-home");
  std::strcpy(pair.configuration_owner_node_id, "namo-node-owner");
  pair.authority_epoch = 2;
  pair.generation = 8;
  pair.roster_revision = 2;
  pair.roster_committed = true;
  for (uint8_t index = 0; index < 2; ++index) {
    auto &member = pair.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id,
                index == 0 ? "namo-node-owner" : "namo-node-peer");
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
  }
  SiteDirectorySnapshot compatibility = canonical;
  compatibility.compatibility_mirror = true;
  SiteCoordinatorState no_leader{};

  ParticipantEndpointCache owner_endpoints{};
  ParticipantCredentialRegistry owner_credentials{};
  observe_directory_source(&owner_endpoints, pair, canonical,
                           "namo-node-owner");
  provision_directory_source(&owner_credentials, "namo-node-owner");
  const auto peer = identity("site-home", "namo-node-peer");
  const char *owner_source = site_directory_bootstrap_source(
      peer, pair, compatibility, no_leader, owner_endpoints,
      owner_credentials, 1000);
  require(owner_source != nullptr &&
              std::strcmp(owner_source, "namo-node-owner") == 0,
          "a two-node Site without a leader must recover from its canonical owner");

  std::strcpy(pair.configuration_owner_node_id, "namo-node-peer");
  ParticipantEndpointCache peer_endpoints{};
  ParticipantCredentialRegistry peer_credentials{};
  observe_directory_source(&peer_endpoints, pair, canonical,
                           "namo-node-owner");
  provision_directory_source(&peer_credentials, "namo-node-owner");
  const char *peer_source = site_directory_bootstrap_source(
      peer, pair, compatibility, no_leader, peer_endpoints,
      peer_credentials, 1000);
  require(peer_source != nullptr &&
              std::strcmp(peer_source, "namo-node-owner") == 0,
          "a reset owner must recover from the sole canonical peer in a two-node Site");

  SiteCoordinatorState stale_local_leader{};
  stale_local_leader.local_leader = true;
  std::strcpy(stale_local_leader.leader_node_id, peer.node_id);
  const char *stale_leader_source = site_directory_bootstrap_source(
      peer, pair, compatibility, stale_local_leader, peer_endpoints,
      peer_credentials, 1000);
  require(stale_leader_source != nullptr &&
              std::strcmp(stale_leader_source, "namo-node-owner") == 0,
          "a reset two-node owner must not deadlock on its stale local Site Leader lease");

  auto &third = pair.members[2];
  third.occupied = true;
  std::strcpy(third.participant_id, "namo-node-third");
  third.type = ParticipantType::NAMO_NODE;
  third.membership = ParticipantMembership::ACTIVE;
  third.leader_eligible = true;
  require(site_directory_bootstrap_source(
              peer, pair, compatibility, no_leader, peer_endpoints,
              peer_credentials, 1000) == nullptr,
          "three or more active nodes without a leader must remain behind the Directory quorum fence");
}

void initial_directory_bootstrap_source_is_read_only_and_bounded() {
  const auto source = identity("site-home", "namo-node-owner");
  const auto directory = replication_directory();
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, source.node_id);
  configuration.roster_committed = true;
  auto &owner = configuration.members[0];
  owner.occupied = true;
  std::strcpy(owner.participant_id, source.node_id);
  owner.type = ParticipantType::NAMO_NODE;
  owner.membership = ParticipantMembership::ACTIVE;
  auto &requester = configuration.members[1];
  requester.occupied = true;
  std::strcpy(requester.participant_id, "namo-node-peer");
  requester.type = ParticipantType::NAMO_NODE;
  requester.membership = ParticipantMembership::QUARANTINED;

  SiteCoordinatorState leader{};
  leader.local_leader = true;
  std::strcpy(leader.leader_node_id, source.node_id);
  ParticipantEndpointCache bootstrap_endpoints{};
  require(site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              leader, bootstrap_endpoints, 1000),
          "the Site Leader must serve canonical Directory state to its quarantined member");

  SiteCoordinatorState no_leader{};
  require(site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              no_leader, bootstrap_endpoints, 1000),
          "the canonical owner must break the bounded initial two-node deadlock");

  requester.membership = ParticipantMembership::ACTIVE;
  std::strcpy(configuration.configuration_owner_node_id,
              requester.participant_id);
  SiteCoordinatorState requester_leader{};
  std::strcpy(requester_leader.leader_node_id, requester.participant_id);
  require(site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              requester_leader, bootstrap_endpoints, 1000),
          "the sole canonical peer must restore a reset owner in a two-node Site");
  std::strcpy(configuration.configuration_owner_node_id, source.node_id);

  auto &third = configuration.members[2];
  third.occupied = true;
  std::strcpy(third.participant_id, "namo-node-third");
  third.type = ParticipantType::NAMO_NODE;
  third.membership = ParticipantMembership::ACTIVE;
  SiteCoordinatorState established{};
  established.configuration_ready = true;
  established.quorum_available = true;
  established.participants_expected = 3;
  std::strcpy(established.leader_node_id, third.participant_id);
  observe_directory_source(&bootstrap_endpoints, configuration, directory,
                           third.participant_id);
  require(site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              established, bootstrap_endpoints, 1000),
          "the Configuration Owner may serve a quarantined joiner when the elected Site Leader confirms the same Directory");
  require(!site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              no_leader, bootstrap_endpoints, 1000),
          "a leaderless three-node Site must not bypass Directory quorum");

  third.membership = ParticipantMembership::REVOKED;
  requester.membership = ParticipantMembership::REVOKED;
  require(!site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, directory,
              no_leader, bootstrap_endpoints, 1000),
          "a revoked member must never receive bootstrap Directory state");

  requester.membership = ParticipantMembership::QUARANTINED;
  SiteDirectorySnapshot compatibility = directory;
  compatibility.compatibility_mirror = true;
  require(!site_directory_bootstrap_request_authorized(
              source, configuration, requester.participant_id, compatibility,
              no_leader, bootstrap_endpoints, 1000),
          "a compatibility mirror must never be served as canonical bootstrap state");

  for (uint8_t active_count = 3; active_count <= 6; ++active_count) {
    SiteConfigurationSnapshot established_configuration{};
    std::strcpy(established_configuration.site_id, source.site_id);
    std::strcpy(established_configuration.configuration_owner_node_id,
                source.node_id);
    established_configuration.authority_epoch = 2;
    established_configuration.generation = 8;
    established_configuration.roster_revision = active_count + 1U;
    established_configuration.roster_committed = true;
    for (uint8_t index = 0; index < active_count; ++index) {
      auto &member = established_configuration.members[index];
      member.occupied = true;
      std::snprintf(member.participant_id, sizeof(member.participant_id),
                    "namo-node-active-%u", static_cast<unsigned>(index));
      member.type = ParticipantType::NAMO_NODE;
      member.membership = ParticipantMembership::ACTIVE;
    }
    std::strcpy(established_configuration.members[0].participant_id,
                source.node_id);
    auto &joining = established_configuration.members[active_count];
    joining.occupied = true;
    std::strcpy(joining.participant_id, "namo-node-joining");
    joining.type = ParticipantType::NAMO_NODE;
    joining.membership = ParticipantMembership::QUARANTINED;

    SiteCoordinatorState established_site{};
    established_site.configuration_ready = true;
    established_site.quorum_available = true;
    established_site.participants_expected = active_count;
    std::strcpy(established_site.leader_node_id,
                established_configuration.members[1].participant_id);
    ParticipantEndpointCache established_endpoints{};
    observe_directory_source(
        &established_endpoints, established_configuration, directory,
        established_site.leader_node_id);
    require(site_directory_bootstrap_request_authorized(
                source, established_configuration, joining.participant_id,
                directory, established_site, established_endpoints, 1000),
            "an Owner confirmed by the elected Site Leader must bootstrap every supported four-to-seven-node enrollment");
  }
}

void mirror_promotion_waits_for_pairing_bootstrap_but_recovers_with_quorum() {
  const auto local = identity("site-home", "namo-node-local");
  SiteDirectorySnapshot mirror = replication_directory();
  mirror.compatibility_mirror = true;

  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, local.site_id);
  std::strcpy(configuration.configuration_owner_node_id, local.node_id);
  configuration.authority_epoch = 2;
  configuration.generation = 8;
  configuration.roster_revision = 2;
  configuration.roster_committed = true;
  auto &local_member = configuration.members[0];
  local_member.occupied = true;
  std::strcpy(local_member.participant_id, local.node_id);
  local_member.type = ParticipantType::NAMO_NODE;
  local_member.membership = ParticipantMembership::ACTIVE;

  SiteCoordinatorState site{};
  site.configuration_ready = true;
  site.local_leader = true;
  std::strcpy(site.leader_node_id, local.node_id);
  ParticipantEndpointCache endpoints{};
  ParticipantCredentialRegistry credentials{};

  require(!site_directory_mirror_promotion_allowed(
              local, configuration, mirror, site, endpoints, credentials,
              1000),
          "an enrolled joiner's temporary one-member mirror must wait for canonical bootstrap");

  for (uint8_t count = 2; count <= 7; ++count) {
    auto recovery = configuration;
    for (uint8_t index = 1; index < count; ++index) {
      auto &peer = recovery.members[index];
      peer.occupied = true;
      std::snprintf(peer.participant_id, sizeof(peer.participant_id),
                    "namo-node-peer-%u", static_cast<unsigned>(index));
      peer.type = ParticipantType::NAMO_NODE;
      peer.membership = ParticipantMembership::ACTIVE;
    }

    auto recovery_site = site;
    recovery_site.participants_expected = count;
    recovery_site.quorum_required = strict_majority(count);
    recovery_site.participants_online = recovery_site.quorum_required;
    recovery_site.quorum_available = true;
    ParticipantEndpointCache recovery_endpoints{};
    ParticipantCredentialRegistry recovery_credentials{};
    for (uint8_t index = 1; index < recovery_site.quorum_required; ++index) {
      const auto &peer = recovery.members[index];
      require(recovery_endpoints.observe_authenticated(
                  peer.participant_id, 0xC0A80102U + index, 49191, 1000) &&
                  recovery_endpoints.observe_floorplan_replica(
                      peer.participant_id, recovery.authority_epoch,
                      recovery.generation,
                      site_configuration_digest(recovery), 0, 0, 0, 0,
                      false),
              "all-mirror recovery endpoint setup failed");
      provision_directory_source(&recovery_credentials,
                                 peer.participant_id);
    }
    require(site_directory_mirror_promotion_allowed(
                local, recovery, mirror, recovery_site, recovery_endpoints,
                recovery_credentials, 1000),
            "an elected Site Leader must recover a two-to-seven-node all-mirror Site with authenticated quorum");

    const auto canonical = replication_directory();
    const auto &canonical_peer = recovery.members[1];
    require(recovery_endpoints.observe_site_directory(
                canonical_peer.participant_id, canonical.topology_revision,
                site_directory_payload_hash(canonical)) &&
                !site_directory_mirror_promotion_allowed(
                    local, recovery, mirror, recovery_site,
                    recovery_endpoints, recovery_credentials, 1000),
            "a canonical peer must always block local mirror promotion during pairing");
  }
}

void matching_site_replication_retries_are_idempotent() {
  SiteReplicationDiagnostics diagnostics{};
  diagnostics.state = SiteReplicationState::WAITING_FOR_OWNER;
  diagnostics.target = SiteReplicationTarget::SITE_CONFIGURATION;
  diagnostics.mode =
      SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT;
  std::strcpy(diagnostics.owner_node_id, "namo-node-owner");
  require(site_replication_session_matches(
              diagnostics, "namo-node-owner",
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT),
          "the same enrollment retry should reuse its active session");
  require(!site_replication_session_matches(
              diagnostics, "namo-node-other",
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT),
          "a different owner must not reuse an active session");
  require(!site_replication_session_matches(
              diagnostics, "namo-node-owner",
              SiteConfigurationReplicationMode::STRICT),
          "a different replication mode must not reuse an active session");
  diagnostics.state = SiteReplicationState::FAILED;
  require(!site_replication_session_matches(
              diagnostics, "namo-node-owner",
              SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT),
          "a failed session must remain restartable");
}

void pairing_site_sync_is_bounded_to_the_unconfigured_pair() {
  const auto owner_id = "namo-node-owner";
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, owner_id);
  configuration.authority_epoch = 2;
  configuration.generation = 7;
  configuration.roster_revision = 4;
  configuration.roster_committed = true;
  configuration.members[0].occupied = true;
  std::strcpy(configuration.members[0].participant_id, "namo-node-local");
  configuration.members[0].type = ParticipantType::NAMO_NODE;
  configuration.members[0].trust = ParticipantTrust::OFFICIAL_VERIFIED;
  configuration.members[0].membership = ParticipantMembership::ACTIVE;
  configuration.members[0].leader_eligible = true;
  configuration.members[1].occupied = true;
  std::strcpy(configuration.members[1].participant_id, owner_id);
  configuration.members[1].type = ParticipantType::NAMO_NODE;
  configuration.members[1].trust = ParticipantTrust::OFFICIAL_VERIFIED;
  configuration.members[1].membership = ParticipantMembership::ACTIVE;
  configuration.members[1].leader_eligible = true;

  ParticipantRecord participant{};
  participant.occupied = true;
  std::strcpy(participant.participant_id, owner_id);
  participant.type = ParticipantType::NAMO_NODE;
  participant.trust = ParticipantTrust::COMMUNITY_UNVERIFIED;
  participant.membership = ParticipantMembership::QUARANTINED;

  ParticipantCredentialRecord credential{};
  credential.occupied = true;
  credential.enabled = true;
  std::strcpy(credential.participant_id, owner_id);

  ParticipantEndpoint endpoint{};
  endpoint.occupied = true;
  endpoint.authenticated = true;
  std::strcpy(endpoint.participant_id, owner_id);
  endpoint.authenticated_at_ms = 1000;
  endpoint.site_configuration_authority_epoch =
      configuration.authority_epoch;
  endpoint.site_configuration_generation = configuration.generation - 1;
  endpoint.site_configuration_digest = 1;

  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, false, 1000) ==
              PairingSiteSyncDecision::START_STRICT,
          "a paired unconfigured node must sync without a Group assignment");

  endpoint.site_configuration_generation = configuration.generation;
  configuration.floorplan.committed = true;
  endpoint.site_configuration_digest = site_configuration_digest(configuration);
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, false, 1000) ==
              PairingSiteSyncDecision::START_STRICT,
          "a missing local Floorplan payload must be repaired");
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, true, 1000) ==
              PairingSiteSyncDecision::ALREADY_CURRENT,
          "a repeated pairing sync must be an idempotent success");

  endpoint.authenticated = false;
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, true, 1000) ==
              PairingSiteSyncDecision::OWNER_OFFLINE,
          "an unauthenticated pairing endpoint must remain unavailable");
  endpoint.authenticated = true;
  endpoint.authenticated_at_ms = 0;
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, true,
              ParticipantEndpointCache::ONLINE_TIMEOUT_MS + 1) ==
              PairingSiteSyncDecision::OWNER_OFFLINE,
          "an expired pairing endpoint must remain unavailable");

  endpoint.authenticated_at_ms = 1000;
  participant.membership = ParticipantMembership::ACTIVE;
  configuration.members[0].membership = ParticipantMembership::QUARANTINED;
  endpoint.site_configuration_generation = configuration.generation - 1;
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, true, 1000) ==
              PairingSiteSyncDecision::START_STRICT,
          "an active direct Owner pair must finish canonical activation sync");

  configuration.members[0].membership = ParticipantMembership::ACTIVE;
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", owner_id, true, 1000) ==
              PairingSiteSyncDecision::NOT_APPLICABLE,
          "a fully activated pair must use the normal Directory route");
  participant.membership = ParticipantMembership::QUARANTINED;
  require(pairing_site_sync_decision(
              configuration, &participant, &credential, &endpoint,
              "namo-node-local", "namo-node-other", true, 1000) ==
              PairingSiteSyncDecision::NOT_APPLICABLE,
          "a different requested owner must not enter the pairing bypass");
}

}  // namespace

void run_site_replication_tests() {
  require(std::strcmp(site_replication_failure_detail_to_string(
                          SiteReplicationFailureDetail::CREDENTIAL_PROBE_REJECTED),
                      "credential_probe_rejected") == 0 &&
              std::strcmp(site_replication_failure_detail_to_string(
                              SiteReplicationFailureDetail::CREDENTIAL_PROBE_TIMEOUT),
                          "credential_probe_timeout") == 0 &&
              std::strcmp(site_replication_failure_detail_to_string(
                              SiteReplicationFailureDetail::CREDENTIAL_BUNDLE_BINDING_MISMATCH),
                          "credential_bundle_binding_mismatch") == 0,
          "credential replication failures must retain distinct diagnostics");
  request_round_trip_keeps_requested_chunk_without_a_body();
  response_round_trip_is_bounded_and_rejects_corruption();
  busy_response_round_trip_is_backward_compatible_and_bodyless();
  response_matching_uses_the_pending_wire_target();
  directory_target_uses_the_existing_bounded_envelope();
  floorplan_manifest_commit_is_owner_only();
  replication_validation_detects_stale_and_split_revisions();
  activation_requires_an_authenticated_matching_floorplan_replica();
  automatic_replication_uses_stable_roster_slots_and_bounded_backoff();
  floorplan_source_lease_serializes_without_a_waiting_queue();
  repeated_replication_loss_stays_bounded_and_releases_the_source();
  replication_replay_cache_is_idempotent_and_bounded();
  canonical_directory_routes_only_to_the_assigned_floorplan_owner();
  replication_route_preserves_legacy_compatibility_and_fails_closed();
  directory_replication_uses_topology_revision_not_local_generation();
  only_the_site_leader_serves_directory_to_active_namo_members();
  authenticated_indirect_site_members_are_admitted();
  quorum_authority_evidence_allows_read_only_replica_sources();
  every_active_namo_member_pulls_the_directory();
  mixed_fallback_policy_replicates_directory_to_seven_nodes();
  initial_directory_bootstrap_is_bounded_and_preserves_the_quorum_fence();
  initial_directory_bootstrap_source_is_read_only_and_bounded();
  mirror_promotion_waits_for_pairing_bootstrap_but_recovers_with_quorum();
  matching_site_replication_retries_are_idempotent();
  pairing_site_sync_is_bounded_to_the_unconfigured_pair();
}

#ifdef NAMO_SITE_REPLICATION_TEST_MAIN
int main() {
  run_site_replication_tests();
  std::puts("site_replication_tests: ok");
  return 0;
}
#endif
