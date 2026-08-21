#include "../../../components/radar_api_server/participant_projection.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "participant_projection_tests: %s\n", message);
  std::exit(1);
}

SiteConfigurationMember &member_at(SiteConfigurationSnapshot *configuration,
                                   std::size_t slot, const char *id,
                                   ParticipantMembership membership) {
  auto &member = configuration->members[slot];
  member.occupied = true;
  std::strcpy(member.participant_id, id);
  member.type = ParticipantType::NAMO_NODE;
  member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
  member.membership = membership;
  member.leader_eligible = true;
  return member;
}

void canonical_roster_survives_empty_successor_registry() {
  SiteConfigurationSnapshot configuration{};
  configuration.roster_committed = true;
  member_at(&configuration, 0, "node-successor", ParticipantMembership::ACTIVE);
  member_at(&configuration, 1, "node-a", ParticipantMembership::ACTIVE);
  member_at(&configuration, 2, "node-b", ParticipantMembership::ACTIVE);

  ParticipantRegistry direct_pairs{};
  require(participant_projection_count("node-successor", &configuration,
                                       direct_pairs) == 2,
          "canonical peers disappeared after Owner handover");
  ParticipantProjection projection{};
  require(participant_projection_at("node-successor", &configuration,
                                    direct_pairs, 0, &projection) &&
              projection.canonical() && !projection.directly_paired() &&
              std::strcmp(projection.participant_id(), "node-a") == 0,
          "successor did not project the first canonical peer");
}

void canonical_state_overrides_stale_direct_pair_state() {
  SiteConfigurationSnapshot configuration{};
  configuration.roster_committed = true;
  member_at(&configuration, 0, "node-owner", ParticipantMembership::ACTIVE);
  member_at(&configuration, 1, "node-peer", ParticipantMembership::ACTIVE);

  ParticipantRegistry direct_pairs{};
  require(direct_pairs.discover("node-peer", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              direct_pairs.revoke("node-peer") == ParticipantRegistryResult::OK,
          "stale direct-pair fixture is invalid");
  ParticipantProjection projection{};
  require(participant_projection_at("node-owner", &configuration,
                                    direct_pairs, 0, &projection) &&
              projection.membership() == ParticipantMembership::ACTIVE &&
              projection.canonical_membership() != nullptr &&
              *projection.canonical_membership() ==
                  ParticipantMembership::ACTIVE &&
              projection.direct_membership() != nullptr &&
              *projection.direct_membership() ==
                  ParticipantMembership::REVOKED &&
              projection.canonical() && projection.directly_paired(),
          "canonical and direct membership sources were not kept separate");
}

void noncanonical_pairing_record_remains_visible_for_cleanup() {
  SiteConfigurationSnapshot configuration{};
  configuration.roster_committed = true;
  member_at(&configuration, 0, "node-owner", ParticipantMembership::ACTIVE);
  member_at(&configuration, 1, "node-active", ParticipantMembership::ACTIVE);

  ParticipantRegistry direct_pairs{};
  require(direct_pairs.discover("node-pending", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "pending direct-pair fixture is invalid");
  ParticipantProjection projection{};
  require(participant_projection_at("node-owner", &configuration,
                                    direct_pairs, 1, &projection) &&
              !projection.canonical() && projection.directly_paired() &&
              projection.canonical_membership() == nullptr &&
              projection.direct_membership() != nullptr &&
              *projection.direct_membership() ==
                  ParticipantMembership::QUARANTINED &&
              projection.membership() == ParticipantMembership::QUARANTINED,
          "noncanonical pending pair was not retained for cleanup");
}

}  // namespace

void run_participant_projection_tests() {
  canonical_roster_survives_empty_successor_registry();
  canonical_state_overrides_stale_direct_pair_state();
  noncanonical_pairing_record_remains_visible_for_cleanup();
}
