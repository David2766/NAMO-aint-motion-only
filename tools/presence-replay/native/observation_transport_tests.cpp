#include "observation_transport.h"
#include "fusion_automation.h"
#include "group_coordinator.h"
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
  std::fprintf(stderr, "observation_transport_tests: %s\n", message);
  std::exit(1);
}

void write_u16_be(uint8_t *target, uint16_t value) {
  target[0] = static_cast<uint8_t>(value >> 8U);
  target[1] = static_cast<uint8_t>(value);
}

void write_u32_be(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value >> 24U);
  target[1] = static_cast<uint8_t>(value >> 16U);
  target[2] = static_cast<uint8_t>(value >> 8U);
  target[3] = static_cast<uint8_t>(value);
}

void write_u64_be(uint8_t *target, uint64_t value) {
  for (int index = 7; index >= 0; --index) {
    target[index] = static_cast<uint8_t>(value);
    value >>= 8U;
  }
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

std::array<uint8_t, 57> minimum_packet(uint64_t token = 0x0102030405060708ULL) {
  std::array<uint8_t, 57> packet{};
  std::memcpy(packet.data(), "NAMO", 4);
  packet[4] = 1;
  packet[5] = 1;
  packet[7] = 24;
  write_u32_be(packet.data() + 8, 1);
  write_u64_be(packet.data() + 12, token);
  write_u16_be(packet.data() + 20, 1);
  packet[24] = 0x80;
  return packet;
}

void envelope_is_strictly_bounded() {
  const auto packet = minimum_packet();
  AuthenticatedObservationEnvelope envelope;
  require(parse_authenticated_observation_envelope(
              packet.data(), packet.size(), &envelope),
          "minimum envelope should parse");
  require(envelope.key_version == 1,
          "key version should use network byte order");
  require(envelope.participant_token == 0x0102030405060708ULL,
          "participant token should use network byte order");
  require(envelope.payload_size == 1 && envelope.payload[0] == 0x80,
          "payload view should point inside the packet");
  require(envelope.message_type ==
              AuthenticatedTransportMessageType::OBSERVATION,
          "legacy packet type must remain observation");
  require(envelope.tag == packet.data() + 25,
          "tag should follow the payload");

  auto site_summary = packet;
  site_summary[5] = static_cast<uint8_t>(
      AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY);
  require(parse_authenticated_observation_envelope(
              site_summary.data(), site_summary.size(), &envelope) &&
              envelope.message_type ==
                  AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY,
          "Site summary must reuse the bounded authenticated envelope");

  auto invalid = packet;
  invalid[0] = 'X';
  require(!parse_authenticated_observation_envelope(
              invalid.data(), invalid.size(), &envelope),
          "wrong magic must be rejected");
  invalid = packet;
  write_u16_be(invalid.data() + 20, 2);
  require(!parse_authenticated_observation_envelope(
              invalid.data(), invalid.size(), &envelope),
          "declared length mismatch must be rejected");
  require(!parse_authenticated_observation_envelope(
              packet.data(), packet.size() - 1, &envelope),
          "truncation must be rejected");
  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PACKET_SIZE + 1>
      oversized{};
  require(!parse_authenticated_observation_envelope(
              oversized.data(), oversized.size(), &envelope),
          "oversized datagrams must be rejected before parsing");
}

void fusion_snapshot_is_bounded_authenticated_and_expiring() {
  SpatialShadowFusionOutput fusion{};
  fusion.sequence = 12;
  fusion.track_count = 3;
  for (uint8_t index = 0; index < fusion.track_count; ++index) {
    auto &track = fusion.tracks[index];
    track.valid = true;
    track.fused_track_id = 4 + index;
    track.state = index == 2 ? SpatialShadowTrackState::COASTING
                             : SpatialShadowTrackState::CONFIRMED;
    track.source_count = 2;
    track.confidence = static_cast<uint8_t>(91 - index);
    track.x_mm = 1234.0f + index * 100.0f;
    track.y_mm = 2345.0f;
    track.vx_mm_s = 120.0f;
    track.vy_mm_s = 0.0f;
  }

  FusionSnapshotEncodeContext context{};
  context.site_id = "site-a";
  context.leader_node_id = "node-a";
  context.group_id = "group-a";
  context.boot_id = "boot-a";
  context.sequence = 7;
  context.group_term = 3;
  context.fusion = &fusion;
  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE>
      payload{};
  std::size_t payload_size = 0;
  require(encode_fusion_snapshot_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::OK,
          "fixed fusion snapshot should encode");
  require(payload_size < AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE,
          "fusion snapshot must fit one bounded datagram");

  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PACKET_SIZE>
      packet{};
  std::memcpy(packet.data() + AuthenticatedObservationEnvelope::PREFIX_SIZE,
              payload.data(), payload_size);
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_authenticated_fusion_snapshot_prefix(
              2, 9, payload_size, packet.data(), packet.size(),
              &signed_size, &packet_size),
          "fusion snapshot envelope should encode");
  AuthenticatedObservationEnvelope envelope{};
  require(parse_authenticated_observation_envelope(
              packet.data(), packet_size, &envelope) &&
              envelope.message_type ==
                  AuthenticatedTransportMessageType::FUSION_SNAPSHOT,
          "fusion snapshot type should round-trip through the envelope");

  FusionSnapshot snapshot{};
  snapshot.occupied = true;
  std::strncpy(snapshot.site_id, "site-a", sizeof(snapshot.site_id) - 1);
  std::strncpy(snapshot.leader_node_id, "node-a",
               sizeof(snapshot.leader_node_id) - 1);
  std::strncpy(snapshot.group_id, "group-a",
               sizeof(snapshot.group_id) - 1);
  std::strncpy(snapshot.boot_id, "boot-a", sizeof(snapshot.boot_id) - 1);
  snapshot.sequence = 7;
  snapshot.group_term = 3;
  snapshot.source_sequence = 12;
  snapshot.received_at_ms = 1000;
  snapshot.expires_at_ms = 3000;
  snapshot.ttl_ms = 2000;
  snapshot.target_count = 1;
  snapshot.presence = true;
  snapshot.motion = true;
  snapshot.targets[0].valid = true;
  snapshot.targets[0].fused_track_id = 4;
  snapshot.targets[0].state = SpatialShadowTrackState::CONFIRMED;
  snapshot.targets[0].source_count = 2;
  snapshot.targets[0].confidence = 91;
  snapshot.targets[0].x_mm = 1234.0f;
  snapshot.targets[0].y_mm = 2345.0f;
  snapshot.targets[0].vx_mm_s = 120.0f;

  FusionSnapshotCache cache;
  require(cache.store(snapshot), "fresh snapshot should enter the cache");
  require(!cache.store(snapshot),
          "duplicate sequence from the same boot must be rejected");
  FusionSnapshot copied{};
  require(cache.copy(2999, &copied) && copied.source_sequence == 12,
          "fresh cached snapshot should be available");
  require(!cache.expire(2999),
          "fresh snapshot must not report an expiration");
  require(cache.expire(3000),
          "the first expired snapshot eviction must be reported");
  require(!cache.copy(3000, &copied),
          "expired snapshot must disappear rather than imply unoccupied");
  require(!cache.expire(3001),
          "an empty cache must not report the same expiration twice");

  FusionSnapshotCache empty_cache;
  require(!empty_cache.expire(3000),
          "an unused cache must not report an expiration");
}

void fusion_snapshot_admission_fails_closed() {
  ParticipantRegistry participants;
  require(
      participants.discover("node-a", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
          participants.classify(
              "node-a", ParticipantTrust::COMMUNITY_UNVERIFIED) ==
              ParticipantRegistryResult::OK &&
          participants.activate("node-a") == ParticipantRegistryResult::OK,
      "snapshot sender fixture should register");

  ParticipantReadinessRegistry readiness;
  ParticipantReadinessConfig config{};
  config.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
      PARTICIPANT_CAP_RANGE | PARTICIPANT_CAP_PRESENCE |
      PARTICIPANT_CAP_MOTION;
  config.max_targets = 3;
  std::strncpy(config.floorplan_id, "floor-a",
               sizeof(config.floorplan_id) - 1);
  std::strncpy(config.space_id, "room-a", sizeof(config.space_id) - 1);
  std::strncpy(config.group_id, "group-a", sizeof(config.group_id) - 1);
  config.geometry_version = 1;
  config.local_pose_version = 1;
  config.pose_version = 1;
  config.calibration_version = 1;
  config.transform.origin_x_mm = 1000;
  config.transform.origin_y_mm = 500;
  config.transform.residual_mm = 20;
  config.transform.sample_count = 2;
  config.transform.valid = true;
  require(readiness.configure("node-a", config) ==
              ParticipantReadinessResult::OK,
          "snapshot sender readiness should register");

  GroupCoordinatorState group{};
  group.configured = true;
  group.term = 3;
  group.local_leader = false;
  std::strncpy(group.group_id, "group-a", sizeof(group.group_id) - 1);
  std::strncpy(group.leader_node_id, "node-a",
               sizeof(group.leader_node_id) - 1);

  FusionSnapshot snapshot{};
  snapshot.occupied = true;
  std::strncpy(snapshot.site_id, "site-a", sizeof(snapshot.site_id) - 1);
  std::strncpy(snapshot.leader_node_id, "node-a",
               sizeof(snapshot.leader_node_id) - 1);
  std::strncpy(snapshot.group_id, "group-a",
               sizeof(snapshot.group_id) - 1);
  std::strncpy(snapshot.boot_id, "boot-a", sizeof(snapshot.boot_id) - 1);
  snapshot.sequence = 1;
  snapshot.group_term = 3;
  snapshot.source_sequence = 1;
  snapshot.ttl_ms = 2000;
  require(evaluate_fusion_snapshot_admission(
              snapshot, "node-a", "site-a", participants, readiness,
              &group) == FusionSnapshotAdmissionResult::OK,
          "current authenticated Group Leader snapshot should be admitted");
  snapshot.group_term = 2;
  require(evaluate_fusion_snapshot_admission(
              snapshot, "node-a", "site-a", participants, readiness,
              &group) == FusionSnapshotAdmissionResult::TERM_MISMATCH,
          "stale Group term must fail closed");
  snapshot.group_term = 3;
  group.local_leader = true;
  require(evaluate_fusion_snapshot_admission(
              snapshot, "node-a", "site-a", participants, readiness,
              &group) == FusionSnapshotAdmissionResult::LEADER_MISMATCH,
          "a local Leader must reject a peer authority snapshot");

  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-a");
  std::strcpy(configuration.configuration_owner_node_id, "node-owner");
  configuration.roster_committed = true;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floor-a");
  configuration.floorplan.geometry_version = 1;
  const char *member_ids[] = {"node-local", "node-a"};
  for (std::size_t index = 0; index < 2; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, member_ids[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-a");
    std::strcpy(member.spatial_profile.group_id, "group-a");
    member.spatial_profile.pose_version = static_cast<uint32_t>(index + 1);
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
  }
  ParticipantRecord canonical_leader{};
  ParticipantReadinessRecord canonical_readiness{};
  require(build_canonical_group_runtime_records(
              &configuration, "node-local", "node-a", &canonical_leader,
              &canonical_readiness),
          "indirect snapshot Leader should project from canonical state");
  group.local_leader = false;
  require(evaluate_fusion_snapshot_admission(
              snapshot, "node-a", "site-a", &canonical_leader,
              &canonical_readiness, &group) ==
              FusionSnapshotAdmissionResult::OK,
          "an indirect canonical Group Leader snapshot should be admitted");
  canonical_leader.membership = ParticipantMembership::REVOKED;
  require(evaluate_fusion_snapshot_admission(
              snapshot, "node-a", "site-a", &canonical_leader,
              &canonical_readiness, &group) ==
              FusionSnapshotAdmissionResult::PARTICIPANT_INACTIVE,
          "an explicit direct revocation must still fence a fusion snapshot");
}

ObservationEncodeContext encode_context(LocalObservation *observation) {
  ObservationEncodeContext context{};
  context.site_id = "site-a";
  context.participant_id = "node-a";
  context.group_id = "group-a";
  context.boot_id = "boot-a";
  context.sequence = 7;
  context.pose_version = 2;
  context.calibration_version = 3;
  context.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_PRESENCE |
      PARTICIPANT_CAP_MOTION;
  context.max_targets = 3;
  context.now_ms = 1025;
  context.observation = observation;
  return context;
}

void observation_encoder_matches_golden_vector() {
  LocalObservation observation{};
  observation.sequence = 1;
  observation.observed_at_ms = 1000;
  observation.ttl_ms = 1500;
  observation.evidence.tracker_presence = true;

  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE>
      payload{};
  std::size_t payload_size = 0;
  auto context = encode_context(&observation);
  require(encode_observation_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::OK,
          "bounded observation should encode");
  require(payload_size == 294 && fnv1a(payload.data(), payload_size) ==
                                     0x39483E59U,
          "MessagePack payload must match the V1 golden vector");

  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PACKET_SIZE>
      packet{};
  std::memcpy(packet.data() + AuthenticatedObservationEnvelope::PREFIX_SIZE,
              payload.data(), payload_size);
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_authenticated_observation_prefix(
              4, 0x0102030405060708ULL, payload_size, packet.data(),
              packet.size(), &signed_size, &packet_size),
          "authenticated envelope prefix should encode");
  require(signed_size ==
              AuthenticatedObservationEnvelope::PREFIX_SIZE + payload_size &&
              packet_size ==
                  signed_size + AuthenticatedObservationEnvelope::TAG_SIZE,
          "authenticated envelope sizes should remain bounded");
  AuthenticatedObservationEnvelope envelope{};
  require(parse_authenticated_observation_envelope(
              packet.data(), packet_size, &envelope) &&
              envelope.key_version == 4 &&
              envelope.participant_token == 0x0102030405060708ULL &&
              envelope.payload_size == payload_size,
          "encoded prefix should round-trip through the strict parser");
}

void observation_encoder_is_bounded_for_three_tracks() {
  LocalObservation observation{};
  observation.sequence = 9;
  observation.observed_at_ms = 2000;
  observation.ttl_ms = 1500;
  observation.evidence.tracker_presence = true;
  observation.evidence.tracker_motion = true;
  observation.track_count = 3;
  for (uint8_t index = 0; index < observation.track_count; ++index) {
    auto &track = observation.tracks[index];
    track.valid = true;
    track.local_track_id = index + 1;
    track.x_mm = 1000.0f + index * 100.0f;
    track.y_mm = 2000.0f;
    track.vx_mm_s = 10.0f;
    track.vy_mm_s = -20.0f;
    track.position_variance_x_mm2 = 10000.0f;
    track.position_variance_y_mm2 = 12000.0f;
    track.distance_mm = 2300.0f;
    track.track_score = 90;
  }
  auto context = encode_context(&observation);
  context.site_id = "site-115b6ed69cc6f75995c1eb37685ddc44";
  context.participant_id = "namo-node-e83dc1faf5f8";
  context.group_id = "fusion-main";
  context.boot_id = "0123456789abcdef";
  context.now_ms = 2025;
  context.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
      PARTICIPANT_CAP_POSITION_UNCERTAINTY | PARTICIPANT_CAP_RANGE |
      PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;

  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE>
      payload{};
  std::size_t payload_size = 0;
  require(encode_observation_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::OK,
          "three full NAMO tracks should fit the fixed payload");
  require(payload_size <= AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE,
          "three-track payload must stay inside one UDP datagram");

  context.now_ms = observation.observed_at_ms + observation.ttl_ms;
  require(encode_observation_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::OBSERVATION_EXPIRED,
          "expired local observations must never be sent");
  context.now_ms = 2025;
  require(encode_observation_messagepack(
              context, payload.data(), 64, &payload_size) ==
              ObservationEncodeResult::BUFFER_TOO_SMALL,
          "encoder must fail closed when the fixed packet budget is too small");

  observation.tracks[0].x_mm = 100001.0f;
  require(encode_observation_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::INVALID_ARGUMENT,
          "sender must reject coordinates the receiver cannot admit");
  observation.tracks[0].x_mm = 1000.0f;
  observation.tracks[0].position_variance_x_mm2 = 1.1e10f;
  require(encode_observation_messagepack(
              context, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::INVALID_ARGUMENT,
          "sender must reject covariance outside the wire contract");
}

void observation_sender_fails_closed_until_peer_is_ready() {
  GroupCoordinatorState group{};
  group.configured = true;
  group.local_leader = false;
  std::strncpy(group.group_id, "fusion-main", sizeof(group.group_id) - 1);
  std::strncpy(group.leader_node_id, "node-b",
               sizeof(group.leader_node_id) - 1);

  ParticipantRecord leader{};
  leader.occupied = true;
  leader.type = ParticipantType::NAMO_NODE;
  leader.membership = ParticipantMembership::ACTIVE;
  std::strncpy(leader.participant_id, "node-b",
               sizeof(leader.participant_id) - 1);

  ParticipantReadinessRecord readiness{};
  readiness.occupied = true;
  std::strncpy(readiness.participant_id, "node-b",
               sizeof(readiness.participant_id) - 1);
  readiness.config.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
      PARTICIPANT_CAP_POSITION_UNCERTAINTY | PARTICIPANT_CAP_RANGE |
      PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;
  readiness.config.max_targets = 3;
  std::strncpy(readiness.config.floorplan_id, "floor-a",
               sizeof(readiness.config.floorplan_id) - 1);
  std::strncpy(readiness.config.space_id, "room-a",
               sizeof(readiness.config.space_id) - 1);
  std::strncpy(readiness.config.group_id, "fusion-main",
               sizeof(readiness.config.group_id) - 1);
  readiness.config.geometry_version = 1;
  readiness.config.local_pose_version = 2;
  readiness.config.pose_version = 3;
  readiness.config.calibration_version = 4;

  ParticipantCredentialRecord credential{};
  credential.occupied = true;
  credential.enabled = true;
  std::strncpy(credential.participant_id, "node-b",
               sizeof(credential.participant_id) - 1);
  credential.participant_token = 7;
  credential.key_version = 1;

  ParticipantEndpoint endpoint{};
  endpoint.occupied = true;
  endpoint.authenticated = true;
  std::strncpy(endpoint.participant_id, "node-b",
               sizeof(endpoint.participant_id) - 1);
  endpoint.ipv4 = 1;
  endpoint.authenticated_at_ms = 1000;

  require(evaluate_observation_send(
              "node-a", &group, false, &leader, &readiness, &credential,
              &endpoint, 2, 1500) == ObservationSendResult::OK,
          "active calibrated online leader should accept shadow sending");
  require(evaluate_observation_send(
              "node-a", &group, true, &leader, &readiness, &credential,
              &endpoint, 2, 1500) ==
              ObservationSendResult::GROUP_UNAVAILABLE,
          "multiple configured groups must fail closed in two-source shadow");
  group.local_leader = true;
  require(evaluate_observation_send(
              "node-a", &group, false, &leader, &readiness, &credential,
              &endpoint, 2, 1500) == ObservationSendResult::LOCAL_LEADER,
          "the elected leader must not send observations to itself");
  group.local_leader = false;
  require(evaluate_observation_send(
              "node-a", &group, false, &leader, &readiness, &credential,
              &endpoint, 1, 1500) == ObservationSendResult::POSE_MISMATCH,
          "stale local pose must block observation sending");
  credential.enabled = false;
  require(evaluate_observation_send(
              "node-a", &group, false, &leader, &readiness, &credential,
              &endpoint, 2, 1500) ==
              ObservationSendResult::CREDENTIAL_UNAVAILABLE,
          "disabled pair credential must block observation sending");
  credential.enabled = true;
  require(evaluate_observation_send(
              "node-a", &group, false, &leader, &readiness, &credential,
              &endpoint, 2,
              1000 + ParticipantEndpointCache::ONLINE_TIMEOUT_MS) ==
              ObservationSendResult::ENDPOINT_OFFLINE,
          "expired heartbeat endpoint must block observation sending");
}

void canonical_group_projection_admits_indirect_observation() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-a");
  std::strcpy(configuration.configuration_owner_node_id, "node-owner");
  configuration.roster_committed = true;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floor-a");
  configuration.floorplan.geometry_version = 1;
  const char *ids[] = {"node-local", "node-indirect"};
  for (std::size_t index = 0; index < 2; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::strcpy(member.participant_id, ids[index]);
    member.type = ParticipantType::NAMO_NODE;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-a");
    std::strcpy(member.spatial_profile.group_id, "group-a");
    member.spatial_profile.pose_version = static_cast<uint32_t>(index + 1);
    member.spatial_profile.calibration_version = 3;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
  }

  ParticipantRecord participant{};
  ParticipantReadinessRecord readiness{};
  require(build_canonical_group_runtime_records(
              &configuration, "node-local", "node-indirect",
              &participant, &readiness),
          "indirect observation fixture should project from canonical state");
  ParticipantObservationMetadata observation{};
  observation.participant_id = "node-indirect";
  observation.participant_type = ParticipantType::NAMO_NODE;
  observation.site_id = "site-a";
  observation.group_id = "group-a";
  observation.boot_id = "boot-indirect";
  observation.sequence = 1;
  observation.ttl_ms = 1500;
  observation.pose_version = 2;
  observation.calibration_version = 3;
  observation.track_count = 1;
  observation.presence = true;
  observation.motion = true;
  observation.has_velocity = true;
  observation.has_position_uncertainty = true;
  observation.has_range = true;

  ObservationAdmission admission{};
  require(admission.admit(&participant, &readiness, "site-a", observation,
                          "group-a") == ObservationAdmissionResult::OK,
          "canonical runtime records must admit an indirect Group observation");
}

void credentials_are_bounded_and_collision_safe() {
  ParticipantCredentialRegistry credentials;
  std::array<uint8_t, 32> key{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id), "satellite-%02u",
                  static_cast<unsigned>(index));
    key[0] = static_cast<uint8_t>(index);
    require(credentials.set(participant_id, 100 + index, 1, key.data()) ==
                ParticipantCredentialResult::OK,
            "credential within the nineteen participant bound should fit");
  }
  require(credentials.size() == ParticipantRegistry::MAX_REMOTE_PARTICIPANTS,
          "credential count should stop at nineteen");
  require(credentials.set("satellite-overflow", 999, 1, key.data()) ==
              ParticipantCredentialResult::FULL,
          "twentieth remote credential must be rejected");

  credentials.clear();
  require(credentials.set("satellite-a", 77, 1, key.data()) ==
              ParticipantCredentialResult::OK,
          "first token should be accepted");
  require(credentials.set("satellite-b", 77, 1, key.data()) ==
              ParticipantCredentialResult::TOKEN_COLLISION,
          "participant token collision must fail closed");
  require(credentials.resolve(77, 2) == nullptr,
          "wrong key version must not resolve");
  auto *resolved = credentials.resolve(77, 1);
  require(resolved != nullptr &&
              std::strcmp(resolved->participant_id, "satellite-a") == 0,
          "registered credential should resolve by token and version");
  require(credentials.erase("satellite-a") == ParticipantCredentialResult::OK &&
              credentials.size() == 0,
          "credential removal should clear authority");
}

void credential_rate_limit_is_per_participant() {
  ObservationTransportRateLimiter limiter;
  require(limiter.allow(1, 1000) && limiter.allow(1, 1500) &&
              limiter.allow(1, 1999),
          "receiver should tolerate one authenticated jitter packet");
  require(!limiter.allow(1, 1999),
          "fourth packet in a window should be limited");
  require(limiter.allow(2, 1999),
          "one participant must not consume another participant's budget");
  require(limiter.allow(1, 2000),
          "a new one-second window should reset the budget");
  require(!limiter.allow(1, 1500),
          "receiver clock regression must fail closed");
}

ObservationTransportSample sample(const char *participant_id, uint64_t token,
                                  uint32_t sequence, uint32_t now_ms) {
  ObservationTransportSample value;
  value.occupied = true;
  std::strncpy(value.participant_id, participant_id,
               ParticipantRecord::MAX_ID_LENGTH);
  value.participant_token = token;
  value.sequence = sequence;
  value.received_at_ms = now_ms;
  value.expires_at_ms = now_ms + 1500;
  value.sample_age_ms = 25;
  value.ttl_ms = 1500;
  value.pose_version = 2;
  value.calibration_version = 3;
  value.track_count = 1;
  value.tracks[0].valid = true;
  value.tracks[0].local_track_id = sequence;
  value.tracks[0].sample_age_ms = 10;
  value.tracks[0].x_mm = static_cast<float>(sequence * 100);
  value.tracks[0].y_mm = 1200.0f;
  value.tracks[0].quality_score = 90;
  return value;
}

void observation_inbox_is_latest_wins_and_expiring() {
  ObservationTransportInbox inbox;
  require(inbox.store(sample("node-a", 11, 1, 1000)),
          "first admitted observation should fit");
  require(inbox.store(sample("node-a", 11, 2, 1100)),
          "new arrival should replace the participant's previous sample");
  require(inbox.size() == 1,
          "latest-wins replacement must not grow the inbox");

  ObservationTransportSample copied;
  require(inbox.copy_latest("node-a", 1200, &copied),
          "fresh latest sample should be readable");
  require(copied.sequence == 2 && copied.tracks[0].local_track_id == 2,
          "copy must expose the newest complete track payload");
  require(copied.sample_age_ms == 125 &&
              copied.tracks[0].sample_age_ms == 110,
          "receiver elapsed time must be added without mutating stored age");
  require(!inbox.copy_latest("node-a", 2600, &copied),
          "expired samples must not be returned");
  inbox.expire(2600);
  require(inbox.size() == 0,
          "explicit expiry must release the participant slot");
}

void observation_inbox_is_bounded_to_remote_participants() {
  ObservationTransportInbox inbox;
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    char participant_id[32]{};
    std::snprintf(participant_id, sizeof(participant_id), "node-%02u",
                  static_cast<unsigned>(index));
    require(inbox.store(
                sample(participant_id, 100 + index, 1, 1000)),
            "each bounded participant slot should accept one sample");
  }
  require(inbox.size() == ParticipantRegistry::MAX_REMOTE_PARTICIPANTS,
          "inbox must stop at the nineteen remote participant bound");
  require(!inbox.store(sample("node-overflow", 999, 1, 1000)),
          "an unrelated twentieth remote sample must be rejected");
}

void fusion_automation_state_is_sequenced_and_fail_closed() {
  FusionAutomationState automation;
  SpatialProductionFusionOutput local{};
  local.build_enabled = true;
  local.authoritative = true;
  local.state = SpatialProductionFusionState::ACTIVE;
  local.source_sequence = 7;
  local.target_count = 1;
  local.targets[0].valid = true;
  local.targets[0].fused_track_id = 3;
  local.targets[0].state = SpatialShadowTrackState::CONFIRMED;
  local.targets[0].source_count = 2;
  local.targets[0].confidence = 90;
  local.targets[0].x_mm = 400.0f;
  local.targets[0].y_mm = 900.0f;

  require(automation.update_local("site-a", "space-a", "group-a",
                                  "node-a", "boot-a", 4, local, 1000),
          "healthy local fusion should create an automation snapshot");
  require(automation.snapshot().authoritative &&
              automation.snapshot().sequence == 7 &&
              automation.snapshot().presence,
          "local automation snapshot should carry authority and occupancy");

  local.source_sequence = 6;
  require(!automation.update_local("site-a", "space-a", "group-a",
                                   "node-a", "boot-a", 4, local, 1100),
          "an older local source sequence must be rejected");
  require(automation.snapshot().sequence == 7,
          "rejected local state must not replace the accepted snapshot");

  automation.mark_unavailable("site-a", "space-a", "group-a", "node-a",
                              4, false);
  require(!automation.snapshot().authoritative &&
              automation.snapshot().health ==
                  FusionAutomationHealth::UNAVAILABLE &&
              automation.snapshot().sequence == 7,
          "transport loss should preserve the last sequence as unavailable");

  local.source_sequence = 8;
  require(automation.update_local("site-a", "space-a", "group-a",
                                  "node-a", "boot-a", 4, local, 1200) &&
              automation.snapshot().authoritative &&
              automation.snapshot().health == FusionAutomationHealth::HEALTHY &&
              automation.snapshot().sequence == 8,
          "newer local fusion must recover automation after temporary loss");
  automation.disable("site-a", "space-a", "group-a", "node-a", 4);
  require(automation.snapshot().health == FusionAutomationHealth::DISABLED &&
              std::strcmp(automation.snapshot().site_id, "site-a") == 0 &&
              std::strcmp(automation.snapshot().space_id, "space-a") == 0 &&
              std::strcmp(automation.snapshot().group_id, "group-a") == 0,
          "disabling fusion must preserve canonical logical identities");
}

void fusion_automation_remote_snapshot_uses_local_frame_and_rejects_stale() {
  FusionAutomationState automation;
  FusionSnapshot remote{};
  remote.occupied = true;
  std::strncpy(remote.site_id, "site-a", sizeof(remote.site_id) - 1);
  std::strncpy(remote.leader_node_id, "node-a",
               sizeof(remote.leader_node_id) - 1);
  std::strncpy(remote.group_id, "group-a", sizeof(remote.group_id) - 1);
  std::strncpy(remote.boot_id, "boot-a", sizeof(remote.boot_id) - 1);
  remote.sequence = 8;
  remote.group_term = 5;
  remote.source_sequence = 12;
  remote.received_at_ms = 1000;
  remote.expires_at_ms = 3000;
  remote.ttl_ms = 2000;
  remote.target_count = 1;
  remote.presence = true;
  remote.motion = true;
  remote.targets[0].valid = true;
  remote.targets[0].fused_track_id = 4;
  remote.targets[0].state = SpatialShadowTrackState::CONFIRMED;
  remote.targets[0].source_count = 2;
  remote.targets[0].confidence = 92;
  remote.targets[0].x_mm = 1400.0f;
  remote.targets[0].y_mm = 900.0f;
  remote.targets[0].vx_mm_s = 100.0f;

  SpatialTransform2D transform{};
  transform.origin_x_mm = 1000;
  transform.valid = 1;
  transform.sample_count = 1;
  require(automation.update_remote(remote, "space-a", transform),
          "fresh remote snapshot should enter automation state");
  require(automation.snapshot().authoritative &&
              automation.snapshot().source ==
                  FusionAutomationSource::GROUP_LEADER_SNAPSHOT &&
              automation.snapshot().fusion.targets[0].x_mm == 400.0f &&
              automation.snapshot().fusion.targets[0].y_mm == -900.0f,
          "remote world coordinates should be converted to the local frame");

  automation.mark_unavailable("site-a", "space-a", "group-a", "node-a",
                              5, false);
  remote.sequence = 7;
  require(!automation.update_remote(remote, "space-a", transform),
          "an older transport sequence must remain rejected after loss");
  require(!automation.snapshot().authoritative &&
              automation.snapshot().sequence == 8,
          "stale input must not turn unavailable state authoritative");

  remote.sequence = 9;
  remote.group_term = 4;
  require(!automation.update_remote(remote, "space-a", transform),
          "an older Group term must be rejected even with a newer sequence");
}

void fusion_automation_holds_only_bounded_read_only_continuity() {
  FusionAutomationState automation;
  SpatialProductionFusionOutput local{};
  local.build_enabled = true;
  local.authoritative = true;
  local.state = SpatialProductionFusionState::ACTIVE;
  local.source_sequence = 21;
  local.target_count = 1;
  local.targets[0].valid = true;
  local.targets[0].fused_track_id = 9;
  local.targets[0].state = SpatialShadowTrackState::CONFIRMED;
  local.targets[0].source_count = 2;
  local.targets[0].confidence = 88;
  local.targets[0].vx_mm_s = 250.0f;
  require(automation.update_local("site-a", "space-a", "group-a",
                                  "node-old", "boot-old", 4, local, 1000),
          "continuity fixture did not accept healthy automation");

  require(automation.preserve_continuity_or_mark_unavailable(
              "site-a", "space-a", "group-a", "node-new", 5, 2000,
              false),
          "a forward Group handoff did not preserve bounded continuity");
  const auto &recovering = automation.snapshot();
  require(!recovering.authoritative && recovering.continuity_held &&
              recovering.health == FusionAutomationHealth::RECOVERING &&
              recovering.presence && !recovering.motion &&
              recovering.fusion.target_count == 1 &&
              std::strcmp(recovering.leader_node_id, "node-new") == 0 &&
              recovering.group_term == 5 &&
              std::strcmp(recovering.continuity_source_leader_node_id,
                          "node-old") == 0 &&
              recovering.continuity_source_group_term == 4,
          "recovering automation did not separate held values from authority");
  require(FusionAutomationState::continuity_remaining_ms(recovering, 2000) ==
              FusionAutomationState::CONTINUITY_HOLD_MS,
          "continuity hold did not start with the bounded duration");
  require(automation.preserve_continuity_or_mark_unavailable(
              "site-a", "space-a", "group-a", "node-new", 5, 31000,
              false) &&
              FusionAutomationState::continuity_remaining_ms(
                  automation.snapshot(), 31000) == 1000,
          "repeated recovery polling extended the original deadline");
  require(!automation.preserve_continuity_or_mark_unavailable(
              "site-a", "space-a", "group-a", "node-new", 5, 32000,
              false),
          "expired continuity remained readable");
  require(!automation.snapshot().continuity_held &&
              automation.snapshot().health ==
                  FusionAutomationHealth::UNAVAILABLE &&
              !automation.snapshot().presence &&
              automation.snapshot().fusion.target_count == 0,
          "expired continuity did not fail closed");

  local.source_sequence = 22;
  require(automation.update_local("site-a", "space-a", "group-a",
                                  "node-new", "boot-new", 5, local, 33000),
          "fresh authority did not replace expired continuity");
  require(!automation.mark_recovering("site-a", "different-space",
                                      "group-a", "node-next", 6, 34000),
          "continuity crossed a logical Space boundary");

  FusionAutomationState cold_start;
  require(!cold_start.preserve_continuity_or_mark_unavailable(
              "site-a", "space-a", "group-a", "node-new", 5, 1000,
              false) &&
              !cold_start.snapshot().continuity_held &&
              cold_start.snapshot().health ==
                  FusionAutomationHealth::UNAVAILABLE,
          "a cold start exposed continuity without a prior healthy snapshot");
}

}  // namespace

void run_observation_transport_tests() {
  envelope_is_strictly_bounded();
  observation_encoder_matches_golden_vector();
  observation_encoder_is_bounded_for_three_tracks();
  fusion_snapshot_is_bounded_authenticated_and_expiring();
  fusion_snapshot_admission_fails_closed();
  observation_sender_fails_closed_until_peer_is_ready();
  canonical_group_projection_admits_indirect_observation();
  credentials_are_bounded_and_collision_safe();
  credential_rate_limit_is_per_participant();
  observation_inbox_is_latest_wins_and_expiring();
  observation_inbox_is_bounded_to_remote_participants();
  fusion_automation_state_is_sequenced_and_fail_closed();
  fusion_automation_remote_snapshot_uses_local_frame_and_rejects_stale();
  fusion_automation_holds_only_bounded_read_only_continuity();
}
