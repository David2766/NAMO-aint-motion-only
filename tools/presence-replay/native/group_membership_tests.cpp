#include "group_membership.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "group_membership_tests: %s\n", message);
  std::exit(1);
}

void add_member(SiteConfigurationSnapshot *configuration, std::size_t slot,
                const char *participant_id, const char *group_id,
                const char *space_id = "room-main",
                bool leader_eligible = true) {
  auto &member = configuration->members[slot];
  member.occupied = true;
  std::strcpy(member.participant_id, participant_id);
  member.type = ParticipantType::NAMO_NODE;
  member.membership = ParticipantMembership::ACTIVE;
  member.leader_eligible = leader_eligible;
  member.spatial_profile.committed = true;
  std::strcpy(member.spatial_profile.group_id, group_id);
  std::strcpy(member.spatial_profile.space_id, space_id);
  member.spatial_profile.pose_version = 1;
  member.spatial_profile.calibration_version = 1;
  member.spatial_profile.transform.valid = true;
  member.spatial_profile.transform.sample_count = 1;
}

SiteConfigurationSnapshot configuration_fixture() {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-test");
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-00");
  configuration.generation = 5;
  configuration.roster_revision = 4;
  configuration.roster_committed = true;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floor-main");
  configuration.floorplan.geometry_version = 1;
  return configuration;
}

void test_local_group_projection_excludes_other_groups() {
  auto configuration = configuration_fixture();
  add_member(&configuration, 0, "namo-node-00", "group-main");
  add_member(&configuration, 1, "namo-node-01", "group-main", "room-main",
             false);
  add_member(&configuration, 2, "namo-node-other", "group-other",
             "room-other");

  GroupMembershipView view{};
  require(build_local_group_membership(&configuration, "namo-node-00",
                                       &view) ==
              GroupMembershipResult::OK,
          "canonical local Group should project");
  require(view.valid && view.local_admitted && !view.over_capacity &&
              view.member_count == 2 && view.configured_member_count == 2,
          "local Group counts should exclude another Group");
  require(view.contains("namo-node-00") &&
              view.contains("namo-node-01") &&
              !view.contains("namo-node-other"),
          "only canonical local Group members should be admitted");
  require(std::strcmp(view.floorplan_id, "floor-main") == 0 &&
              std::strcmp(view.space_id, "room-main") == 0 &&
              std::strcmp(view.group_id, "group-main") == 0 &&
              view.assignment_revision == 4,
          "projection metadata should come from canonical configuration");
  require(view.find("namo-node-01") != nullptr &&
              !view.find("namo-node-01")->leader_eligible,
          "leader eligibility must remain canonical");
}

void test_eighth_member_does_not_displace_the_admitted_seven() {
  auto configuration = configuration_fixture();
  char participant_id[32]{};
  for (std::size_t index = 0; index < 7; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_member(&configuration, index, participant_id, "group-main");
  }

  GroupMembershipView before{};
  require(build_local_group_membership(&configuration, "namo-node-00",
                                       &before) ==
              GroupMembershipResult::OK,
          "seven-member Group should project");
  require(before.member_count == 7 && !before.over_capacity,
          "seven members should fill the Group exactly");

  add_member(&configuration, 7, "namo-node-07", "group-main");
  GroupMembershipView after{};
  require(build_local_group_membership(&configuration, "namo-node-00",
                                       &after) ==
              GroupMembershipResult::OK,
          "existing member should stay admitted after an eighth member");
  require(after.member_count == 7 && after.configured_member_count == 8 &&
              after.over_capacity && !after.contains("namo-node-07"),
          "the eighth member must be excluded from the bounded runtime");
  for (std::size_t index = 0; index < before.member_count; ++index) {
    require(std::strcmp(before.members[index].participant_id,
                        after.members[index].participant_id) == 0,
            "adding an eighth member must not reorder admitted members");
  }
}

void test_excluded_local_member_fails_closed() {
  auto configuration = configuration_fixture();
  char participant_id[32]{};
  for (std::size_t index = 0; index < 8; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_member(&configuration, index, participant_id, "group-main");
  }

  GroupMembershipView view{};
  require(build_local_group_membership(&configuration, "namo-node-07",
                                       &view) ==
              GroupMembershipResult::LOCAL_NOT_ADMITTED,
          "an eighth local member must fail closed");
  require(view.valid && view.over_capacity && !view.local_admitted &&
              !view.contains("namo-node-07"),
          "excluded local member must not enter the high-frequency runtime");
}

void test_missing_canonical_assignment_stays_unavailable() {
  auto configuration = configuration_fixture();
  add_member(&configuration, 0, "namo-node-00", "group-main");
  configuration.members[0].spatial_profile.committed = false;

  GroupMembershipView view{};
  require(build_local_group_membership(&configuration, "namo-node-00",
                                       &view) ==
              GroupMembershipResult::LOCAL_ASSIGNMENT_UNAVAILABLE,
          "missing canonical assignment should not be guessed");
  require(!view.valid && view.member_count == 0,
          "unassigned Group view must remain empty");
}

void test_canonical_runtime_projection_replaces_pair_local_readiness() {
  auto configuration = configuration_fixture();
  add_member(&configuration, 0, "namo-node-00", "group-main");
  add_member(&configuration, 1, "namo-node-01", "group-main");
  configuration.members[0].spatial_profile.pose_version = 7;
  configuration.members[1].spatial_profile.pose_version = 11;
  configuration.members[1].spatial_profile.calibration_version = 13;
  configuration.members[1].spatial_profile.transform.origin_x_mm = 900;

  ParticipantRecord participant{};
  ParticipantReadinessRecord readiness{};
  require(build_canonical_group_runtime_records(
              &configuration, "namo-node-00", "namo-node-01",
              &participant, &readiness),
          "canonical NAMO member should project without a direct pair record");
  require(std::strcmp(participant.participant_id, "namo-node-01") == 0 &&
              participant.membership == ParticipantMembership::ACTIVE &&
              ParticipantReadinessRegistry::ready(&readiness),
          "runtime projection must remain an active ready NAMO participant");
  require(readiness.config.local_pose_version == 7 &&
              readiness.config.pose_version == 11 &&
              readiness.config.calibration_version == 13 &&
              readiness.config.max_targets == 3 &&
              readiness.config.transform.origin_x_mm == 900,
          "runtime projection must preserve canonical spatial versions and transform");

  configuration.members[1].type = ParticipantType::SATELLITE;
  require(!build_canonical_group_runtime_records(
              &configuration, "namo-node-00", "namo-node-01",
              &participant, &readiness),
          "Satellite capabilities must not be guessed from a NAMO contract");
}

}  // namespace

void run_group_membership_tests() {
  test_local_group_projection_excludes_other_groups();
  test_eighth_member_does_not_displace_the_admitted_seven();
  test_excluded_local_member_fails_closed();
  test_missing_canonical_assignment_stays_unavailable();
  test_canonical_runtime_projection_replaces_pair_local_readiness();
}
