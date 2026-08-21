#include "site_space_summary.h"
#include "observation_transport.h"
#include "spatial_production_fusion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_space_summary_tests: %s\n", message);
  std::exit(1);
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

SiteSpaceSummary make_summary(const char *group_id = "group-a",
                              uint32_t sequence = 1) {
  SiteSpaceSummary summary{};
  summary.occupied = true;
  copy_text(summary.site_id, sizeof(summary.site_id), "site-a");
  copy_text(summary.space_id, sizeof(summary.space_id), "room-a");
  copy_text(summary.group_id, sizeof(summary.group_id), group_id);
  copy_text(summary.leader_node_id, sizeof(summary.leader_node_id), "node-a");
  copy_text(summary.boot_id, sizeof(summary.boot_id), "boot-a");
  summary.sequence = sequence;
  summary.group_term = 3;
  summary.source_sequence = sequence;
  summary.target_count = 1;
  summary.presence = true;
  return summary;
}

void builder_emits_only_bounded_aggregate_state() {
  SpatialProductionFusionOutput fusion{};
  fusion.build_enabled = true;
  fusion.authoritative = true;
  fusion.state = SpatialProductionFusionState::ACTIVE;
  fusion.source_sequence = 17;
  fusion.target_count = 3;
  for (uint8_t index = 0; index < 3; ++index) {
    auto &target = fusion.targets[index];
    target.valid = true;
    target.fused_track_id = index + 1;
    target.state = SpatialShadowTrackState::CONFIRMED;
    target.source_count = 2;
    target.confidence = 90;
    target.x_mm = static_cast<float>(index) * 400.0f;
    target.y_mm = 2000.0f;
  }

  SiteSpaceSummary summary{};
  require(build_site_space_summary(
              "site-a", "room-a", "group-a", "node-a", "boot-a", 4, 3,
              fusion, &summary),
          "authoritative output should build a Site summary");
  require(summary.target_count == 3 && summary.presence,
          "summary should retain only bounded aggregate occupancy");
}

void admission_requires_the_current_group_leader() {
  SiteConfigurationSnapshot configuration{};
  auto &member = configuration.members[0];
  member.occupied = true;
  member.membership = ParticipantMembership::ACTIVE;
  member.leader_eligible = true;
  copy_text(member.participant_id, sizeof(member.participant_id), "node-a");
  member.spatial_profile.committed = true;
  copy_text(member.spatial_profile.space_id,
            sizeof(member.spatial_profile.space_id), "room-a");
  copy_text(member.spatial_profile.group_id,
            sizeof(member.spatial_profile.group_id), "group-a");

  SiteCoordinatorState site{};
  site.local_leader = true;
  copy_text(site.leader_node_id, sizeof(site.leader_node_id), "site-leader");
  GroupCoordinatorState group{};
  group.configured = true;
  group.term = 3;
  copy_text(group.group_id, sizeof(group.group_id), "group-a");
  copy_text(group.leader_node_id, sizeof(group.leader_node_id), "node-a");
  const auto summary = make_summary();
  require(evaluate_site_space_summary_admission(
              summary, "node-a", "site-leader", "site-a", site,
              &configuration, &group) == SiteSpaceSummaryAdmissionResult::OK,
          "authenticated current Group Leader should be admitted");
  group.term = 4;
  require(evaluate_site_space_summary_admission(
              summary, "node-a", "site-leader", "site-a", site,
              &configuration, &group) ==
              SiteSpaceSummaryAdmissionResult::GROUP_TERM_MISMATCH,
          "stale Group terms must fail closed");
  group.term = 3;
  copy_text(group.leader_node_id, sizeof(group.leader_node_id), "node-b");
  require(evaluate_site_space_summary_admission(
              summary, "node-a", "site-leader", "site-a", site,
              &configuration, &group) ==
              SiteSpaceSummaryAdmissionResult::GROUP_LEADER_MISMATCH,
          "a former Group Leader must not publish summaries");
}

void wire_payload_stays_inside_one_authenticated_datagram() {
  const auto summary = make_summary();
  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE>
      payload{};
  std::size_t payload_size = 0;
  require(encode_site_space_summary_messagepack(
              summary, payload.data(), payload.size(), &payload_size) ==
              ObservationEncodeResult::OK,
          "aggregate Space summary should encode");
  require(payload_size < AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE,
          "aggregate Space summary must fit one datagram");
  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PACKET_SIZE>
      packet{};
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  require(encode_authenticated_site_space_summary_prefix(
              1, 9, payload_size, packet.data(), packet.size(), &signed_size,
              &packet_size),
          "aggregate Space summary should use the authenticated envelope");
  AuthenticatedObservationEnvelope envelope{};
  require(parse_authenticated_observation_envelope(
              packet.data(), packet_size, &envelope) &&
              envelope.message_type ==
                  AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY,
          "aggregate Space summary envelope should round-trip");
}

void cache_is_bounded_ordered_and_expiring() {
  SiteSpaceSummaryCache cache;
  auto first = make_summary();
  require(cache.store(first, 1000) == SiteSpaceSummaryAdmissionResult::OK,
          "first summary should enter the cache");
  require(cache.store(first, 1100) ==
              SiteSpaceSummaryAdmissionResult::DUPLICATE_OR_REORDERED,
          "duplicate sequence must be rejected");
  first.sequence = 2;
  first.source_sequence = 2;
  require(cache.store(first, 1200) == SiteSpaceSummaryAdmissionResult::OK,
          "newer sequence should replace the cached value");

  for (uint8_t index = 1; index < SiteSpaceSummaryCache::MAX_SPACES; ++index) {
    char group_id[24]{};
    std::snprintf(group_id, sizeof(group_id), "group-%u", index);
    auto summary = make_summary(group_id, index + 2);
    summary.source_sequence = index + 2;
    require(cache.store(summary, 1200) == SiteSpaceSummaryAdmissionResult::OK,
            "cache should accept all eight bounded spaces");
  }
  auto overflow = make_summary("group-overflow", 20);
  require(cache.store(overflow, 1200) == SiteSpaceSummaryAdmissionResult::FULL,
          "ninth space must be rejected instead of allocating");
  cache.expire(1200 + SiteSpaceSummary::DEFAULT_TTL_MS);
  require(cache.size() == 0 && cache.expired() == 8,
          "expired summaries must leave no stale occupancy state");
}

}  // namespace

void run_site_space_summary_tests() {
  builder_emits_only_bounded_aggregate_state();
  admission_requires_the_current_group_leader();
  wire_payload_stays_inside_one_authenticated_datagram();
  cache_is_bounded_ordered_and_expiring();
}
