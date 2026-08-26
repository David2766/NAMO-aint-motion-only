#include "group_coordinator.h"
#include "participant_credentials.h"
#include "site_directory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "group_coordinator_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity identity(const char *node_id) {
  PlatformIdentity value{};
  std::strcpy(value.site_id, "site-test");
  std::strcpy(value.node_id, node_id);
  return value;
}

ParticipantReadinessConfig spatial_config(const char *group_id) {
  ParticipantReadinessConfig config{};
  config.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                            PARTICIPANT_CAP_VELOCITY |
                            PARTICIPANT_CAP_RANGE |
                            PARTICIPANT_CAP_PRESENCE |
                            PARTICIPANT_CAP_MOTION;
  config.max_targets = 3;
  std::strcpy(config.floorplan_id, "floor-main");
  std::strcpy(config.space_id, "room-main");
  std::strcpy(config.group_id, group_id);
  config.geometry_version = 1;
  config.local_pose_version = 1;
  config.pose_version = 1;
  config.calibration_version = 1;
  config.transform.residual_mm = 0;
  config.transform.sample_count = 1;
  config.transform.valid = true;
  return config;
}

void add_active(ParticipantRegistry *participants,
                ParticipantReadinessRegistry *readiness,
                const char *participant_id, const char *group_id,
                ParticipantType type = ParticipantType::NAMO_NODE,
                ParticipantTrust trust = ParticipantTrust::OFFICIAL_VERIFIED) {
  require(participants->discover(participant_id, type) ==
              ParticipantRegistryResult::OK,
          "participant discovery failed");
  require(participants->classify(participant_id, trust) ==
              ParticipantRegistryResult::OK,
          "participant classification failed");
  require(readiness->configure(participant_id, spatial_config(group_id)) ==
              ParticipantReadinessResult::OK,
          "participant readiness failed");
  require(participants->activate(participant_id) == ParticipantRegistryResult::OK,
          "participant activation failed");
}

GroupHeartbeat heartbeat(const char *participant_id, const char *group_id,
                         const char *boot_id, uint32_t sequence,
                         uint32_t term = 1, uint32_t lease_ms = 6000,
                         uint32_t age_ms = 0) {
  GroupHeartbeat value{};
  value.participant_id = participant_id;
  value.group_id = group_id;
  value.boot_id = boot_id;
  value.sequence = sequence;
  value.term = term;
  value.lease_ms = lease_ms;
  value.sample_age_ms = age_ms;
  return value;
}

const GroupCoordinatorState *only_group(const GroupCoordinator &coordinator) {
  require(coordinator.group_count() == 1, "expected exactly one group");
  for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
    const auto *group = coordinator.group_at(slot);
    if (group != nullptr)
      return group;
  }
  return nullptr;
}

SiteConfigurationSnapshot canonical_configuration(
    const PlatformIdentity &owner, const ParticipantRegistry &participants) {
  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "canonical configuration bootstrap failed");
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, participants, base,
                                   &configuration) ==
              SiteConfigurationResult::OK,
          "canonical configuration commit failed");
  return configuration;
}

void assign_canonical_group(SiteConfigurationSnapshot *configuration,
                            const char *group_id) {
  require(configuration != nullptr && group_id != nullptr,
          "canonical Group assignment fixture is invalid");
  for (auto &member : configuration->members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, group_id);
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
  }
}

void test_standalone_uses_the_same_authority_contract() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  coordinator.update(identity("namo-node-local"), participants, readiness, 1000);
  const auto *group = only_group(coordinator);
  require(group != nullptr && !group->configured,
          "standalone group should be unconfigured");
  require(group->local_leader && group->term == 1,
          "standalone node should hold term one");
  require(group->participants_expected == 1 && group->participants_online == 1,
          "standalone counts should include only the local node");
  require(GroupCoordinator::lease_remaining_ms(*group, 1000) ==
              GroupCoordinator::DEFAULT_LEASE_MS,
          "standalone lease should use the normal lease contract");
}

void test_standalone_slot_is_replaced_by_canonical_group() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  GroupCoordinator coordinator{};

  coordinator.update(local, participants, readiness, 1000);
  require(!only_group(coordinator)->configured,
          "transition fixture must begin standalone");

  add_active(&participants, &readiness, "namo-node-peer", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 2000);

  const auto *group = only_group(coordinator);
  require(group->configured &&
              std::strcmp(group->group_id, "group-main") == 0 &&
              group->participants_expected == 2,
          "canonical Group must replace the previous standalone slot");
}

void test_configured_slot_tracks_canonical_group_change() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-peer", "group-a");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-a");
  GroupCoordinator coordinator{};

  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(std::strcmp(only_group(coordinator)->group_id, "group-a") == 0,
          "transition fixture must begin in Group A");

  assign_canonical_group(&configuration, "group-b");
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 2000);

  const auto *group = only_group(coordinator);
  require(group->configured && std::strcmp(group->group_id, "group-b") == 0 &&
              group->participants_expected == 2,
          "canonical Group B must replace the stale Group A slot");
}

void test_configured_slot_returns_to_standalone() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-peer", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");
  GroupCoordinator coordinator{};

  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(only_group(coordinator)->configured,
          "transition fixture must begin configured");

  ParticipantRegistry empty_participants{};
  ParticipantReadinessRegistry empty_readiness{};
  coordinator.update(local, empty_participants, empty_readiness, 2000);

  const auto *group = only_group(coordinator);
  require(!group->configured && group->group_id[0] == '\0' &&
              group->local_leader && group->participants_expected == 1 &&
              group->participants_online == 1,
          "standalone scope must replace a removed canonical Group");
}

void test_standalone_slot_accepts_seven_member_canonical_group() {
  const auto owner = identity("namo-node-00-owner");
  const auto local = identity("namo-node-06-local");
  ParticipantRegistry owner_participants{};
  ParticipantReadinessRegistry owner_readiness{};
  add_active(&owner_participants, &owner_readiness, local.node_id,
             "group-main");
  char participant_id[32]{};
  for (std::size_t index = 1; index <= 5; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_active(&owner_participants, &owner_readiness, participant_id,
               "group-main");
  }
  auto configuration = canonical_configuration(owner, owner_participants);
  assign_canonical_group(&configuration, "group-main");

  ParticipantRegistry local_participants{};
  ParticipantReadinessRegistry local_readiness{};
  add_active(&local_participants, &local_readiness, owner.node_id,
             "group-main");
  for (std::size_t index = 1; index <= 5; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_active(&local_participants, &local_readiness, participant_id,
               "group-main");
  }

  GroupCoordinator coordinator{};
  ParticipantRegistry empty_participants{};
  ParticipantReadinessRegistry empty_readiness{};
  coordinator.update(local, empty_participants, empty_readiness, 1000);
  coordinator.update(local, local_participants, local_readiness,
                     &configuration, "namo-node-01", 2000);

  const auto *membership = coordinator.local_membership();
  const auto *group = only_group(coordinator);
  require(membership != nullptr && membership->member_count == 7 &&
              !membership->over_capacity && group->configured &&
              std::strcmp(group->group_id, "group-main") == 0 &&
              group->participants_expected == 7,
          "a split-role seven-member Group must replace standalone in one update");
}

void test_eligible_remote_leader_and_lease_expiry() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, readiness, 1000);
  const auto *group = only_group(coordinator);
  require(group->local_leader && group->participants_expected == 2 &&
              group->participants_online == 1,
          "offline remote should not win or count online");

  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 1), 1000) ==
              GroupHeartbeatResult::OK,
          "valid heartbeat should be admitted");
  coordinator.update(local, participants, readiness, 1000);
  group = only_group(coordinator);
  require(!group->local_leader && group->leader_node_id[0] == '\0' &&
              group->participants_online == 2 && group->term == 1,
          "a follower must wait for the selected remote winner to issue its term");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 2, 2), 1100) ==
              GroupHeartbeatResult::OK,
          "the selected remote winner term should be admitted");
  coordinator.update(local, participants, readiness, 1100);
  group = only_group(coordinator);
  require(!group->local_leader &&
              std::strcmp(group->leader_node_id, "namo-node-a") == 0 &&
              group->participants_online == 2 && group->term == 2,
          "the selected remote winner term should establish authority");

  coordinator.update(local, participants, readiness, 7100);
  group = only_group(coordinator);
  require(group->local_leader &&
              std::strcmp(group->leader_node_id, "namo-node-z") == 0 &&
              group->participants_online == 1 && group->term == 3,
          "expired remote lease should return authority to the local node");
}

void test_sequence_and_boot_rules() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, readiness, 1000);

  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 5), 1000) ==
              GroupHeartbeatResult::OK,
          "initial sequence should be accepted");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 5), 1100) ==
              GroupHeartbeatResult::DUPLICATE_OR_REORDERED,
          "duplicate sequence should be rejected");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 4), 1100) ==
              GroupHeartbeatResult::DUPLICATE_OR_REORDERED,
          "reordered sequence should be rejected");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-b", 1), 1100) ==
              GroupHeartbeatResult::OK,
          "new boot id should reset sequence tracking");
}

void test_leader_eligibility_is_explicit() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main",
             ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, readiness, 1000);
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 1), 1000) ==
              GroupHeartbeatResult::OK,
          "community heartbeat should be admitted");
  coordinator.update(local, participants, readiness, 1000);
  require(only_group(coordinator)->local_leader,
          "community node should not lead without opt-in");

  require(participants.set_community_leader_eligible("namo-node-a", true) ==
              ParticipantRegistryResult::OK,
          "community leader opt-in failed");
  coordinator.update(local, participants, readiness, 1100);
  require(!only_group(coordinator)->local_leader,
          "opted-in community node should be eligible");
}

void test_satellite_can_be_online_but_never_lead() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "satellite-a", "group-main",
             ParticipantType::SATELLITE, ParticipantTrust::SATELLITE);
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, readiness, 1000);
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("satellite-a", "group-main", "boot-a", 1), 1000) ==
              GroupHeartbeatResult::OK,
          "satellite heartbeat should be admitted");
  coordinator.update(local, participants, readiness, 1000);
  const auto *group = only_group(coordinator);
  require(group->local_leader,
          "satellite should never replace the local NAMO Group Leader");
  require(group->participants_online == 2,
          "satellite heartbeat should contribute Group health");
}

void test_admission_rejects_wrong_or_stale_input() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, readiness, 1000);

  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("unknown-node", "group-main", "boot-a", 1), 1000) ==
              GroupHeartbeatResult::UNKNOWN_PARTICIPANT,
          "unknown participant should be rejected");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-other", "boot-a", 1), 1000) ==
              GroupHeartbeatResult::GROUP_MISMATCH,
          "wrong group should be rejected");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 1, 1, 6000,
                        6000),
              1000) == GroupHeartbeatResult::EXPIRED,
          "expired heartbeat should be rejected");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 1, 3), 1000) ==
              GroupHeartbeatResult::OK,
          "higher term should advance the shadow coordinator");
  coordinator.update(local, participants, readiness, 1000);
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 2, 2), 1100) ==
              GroupHeartbeatResult::STALE_TERM,
          "older term should be rejected");
  require(participants.quarantine("namo-node-a") == ParticipantRegistryResult::OK,
          "participant quarantine failed");
  require(coordinator.accept_heartbeat(
              participants, readiness,
              heartbeat("namo-node-a", "group-main", "boot-a", 2, 3), 1100) ==
              GroupHeartbeatResult::PARTICIPANT_INACTIVE,
          "inactive participant should be rejected");
}

void test_multiple_remote_groups_without_local_assignment_fail_closed() {
  GroupCoordinator coordinator{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  char participant_id[32]{};
  char group_id[32]{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    std::snprintf(group_id, sizeof(group_id), "group-%02u",
                  static_cast<unsigned>(index));
    add_active(&participants, &readiness, participant_id, group_id);
  }
  coordinator.update(identity("namo-node-local"), participants, readiness, 1000);
  require(coordinator.group_count() == 1 &&
              only_group(coordinator)->configured == false,
          "a Node without a local assignment must not select one of several "
          "remote Groups");
}

void test_group_leader_avoids_site_and_configuration_roles() {
  const auto owner = identity("namo-node-owner");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-site", "group-main");
  add_active(&participants, &readiness, "namo-node-worker", "group-main");
  auto configuration = canonical_configuration(owner, participants);
  assign_canonical_group(&configuration, "group-main");

  GroupCoordinator coordinator{};
  coordinator.update(owner, participants, readiness, &configuration,
                     "namo-node-site", 1000);
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration,
              heartbeat("namo-node-site", "group-main", "boot-site", 1),
              1000) == GroupHeartbeatResult::OK,
          "Site Leader heartbeat should be admitted");
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration,
              heartbeat("namo-node-worker", "group-main", "boot-worker", 1),
              1000) == GroupHeartbeatResult::OK,
          "ordinary Group candidate heartbeat should be admitted");
  coordinator.update(owner, participants, readiness, &configuration,
                     "namo-node-site", 1000);

  const auto *group = only_group(coordinator);
  require(!group->local_leader &&
              std::strcmp(group->leader_node_id, "namo-node-worker") == 0,
          "an ordinary candidate should lead before Site or configuration roles");
}

void test_configuration_owner_leads_before_site_leader_fallback() {
  const auto owner = identity("namo-node-z-owner");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a-site", "group-main");
  auto configuration = canonical_configuration(owner, participants);
  assign_canonical_group(&configuration, "group-main");

  GroupCoordinator coordinator{};
  coordinator.update(owner, participants, readiness, &configuration,
                     "namo-node-a-site", 1000);
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration,
              heartbeat("namo-node-a-site", "group-main", "boot-site", 1),
              1000) == GroupHeartbeatResult::OK,
          "fallback Site Leader heartbeat should be admitted");
  coordinator.update(owner, participants, readiness, &configuration,
                     "namo-node-a-site", 1000);

  const auto *group = only_group(coordinator);
  require(group->local_leader &&
              std::strcmp(group->leader_node_id, owner.node_id) == 0,
          "configuration owner should lead before overlapping Site and Group roles");
}

void test_site_leader_can_lead_when_no_other_candidate_exists() {
  const auto owner = identity("namo-node-owner");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "satellite-a", "group-main",
             ParticipantType::SATELLITE, ParticipantTrust::SATELLITE);
  const auto configuration = canonical_configuration(owner, participants);

  GroupCoordinator coordinator{};
  coordinator.update(owner, participants, readiness, &configuration,
                     owner.node_id, 1000);
  const auto satellite_result = coordinator.accept_heartbeat(
      participants, readiness, &configuration,
      heartbeat("satellite-a", "group-main", "boot-satellite", 1), 1000);
  require(satellite_result == GroupHeartbeatResult::OK,
          "satellite heartbeat should contribute Group health");
  coordinator.update(owner, participants, readiness, &configuration,
                     owner.node_id, 1000);

  const auto *group = only_group(coordinator);
  require(group->local_leader &&
              std::strcmp(group->leader_node_id, owner.node_id) == 0,
          "a sole eligible Node must be allowed to hold every role");
}

void test_legacy_twenty_node_group_is_bounded_to_seven() {
  const auto owner = identity("namo-node-owner");
  const auto local = identity("namo-node-00");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  char participant_id[32]{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_active(&participants, &readiness, participant_id, "group-main");
  }
  auto configuration = canonical_configuration(owner, participants);
  assign_canonical_group(&configuration, "group-main");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  const auto *bounded_membership = coordinator.local_membership();
  require(bounded_membership != nullptr &&
              bounded_membership->member_count ==
                  GroupMembershipView::MAX_PARTICIPANTS &&
              bounded_membership->over_capacity,
          "legacy oversized Group must build a bounded membership view");
  for (std::size_t index = 1;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    char boot_id[32]{};
    std::snprintf(boot_id, sizeof(boot_id), "boot-%02u",
                  static_cast<unsigned>(index));
    const auto admitted = coordinator.accept_heartbeat(
        participants, readiness, &configuration,
        heartbeat(participant_id, "group-main", boot_id, 1), 1000);
    require(admitted ==
                (bounded_membership->contains(participant_id)
                     ? GroupHeartbeatResult::OK
                     : GroupHeartbeatResult::GROUP_MISMATCH),
            "legacy oversized Group admission must stop after six remotes");
  }
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);

  const auto *group = only_group(coordinator);
  require(group->participants_expected == GroupMembershipView::MAX_PARTICIPANTS &&
              group->participants_online == GroupMembershipView::MAX_PARTICIPANTS,
          "legacy oversized Group must retain seven-member accounting");
  require(!group->local_leader &&
              std::strcmp(group->leader_node_id, "namo-node-01") == 0,
          "bounded legacy Group should avoid Site and configuration roles deterministically");
}

void test_ambiguous_local_multi_group_assignment_fails_closed() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-room-a", "group-a");
  add_active(&participants, &readiness, "namo-node-room-b", "group-b");
  const auto configuration = canonical_configuration(local, participants);

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(coordinator.group_count() == 1 &&
              only_group(coordinator)->configured == false,
          "multi-Group input without a local assignment must fail closed");
  const auto *group = only_group(coordinator);
  require(group->group_id[0] == '\0' && group->local_leader &&
              std::strcmp(group->leader_node_id, local.node_id) == 0,
          "a local Node with no canonical Group assignment must remain "
          "standalone instead of claiming a remote Group");
}

void test_canonical_local_assignment_selects_only_its_group() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-room-a", "group-a");
  add_active(&participants, &readiness, "namo-node-room-b", "group-b");
  auto configuration = canonical_configuration(local, participants);

  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
    const char *group_id =
        std::strcmp(member.participant_id, "namo-node-room-b") == 0
            ? "group-b"
            : "group-a";
    std::strcpy(member.spatial_profile.space_id, group_id);
    std::strcpy(member.spatial_profile.group_id, group_id);
  }

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(coordinator.group_count() == 1,
          "canonical runtime should retain only the local Group");
  const auto *local_group =
      coordinator.local_group(&configuration, local.node_id);
  require(local_group != nullptr &&
              std::strcmp(local_group->group_id, "group-a") == 0 &&
              !local_group->local_leader &&
              local_group->leader_node_id[0] == '\0' &&
              local_group->participants_expected == 2 &&
              local_group->participants_online == 1 &&
              !local_group->quorum_available,
          "local Node must participate only in its canonical Group");

  for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
    const auto *group = coordinator.group_at(slot);
    require(group == nullptr || std::strcmp(group->group_id, "group-b") != 0,
            "another canonical Group must stay out of the local runtime");
  }
}

void test_canonical_group_capacity_excludes_the_eighth_member() {
  const auto local = identity("namo-node-00");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  char participant_id[32]{};
  for (std::size_t index = 1; index <= 7; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    add_active(&participants, &readiness, participant_id, "group-main");
  }
  auto configuration = canonical_configuration(local, participants);
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
  }

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  const auto *membership = coordinator.local_membership();
  require(coordinator.group_count() == 1,
          "bounded canonical Group should expose one local Group");
  const auto *group =
      coordinator.local_group(&configuration, local.node_id);
  require(membership != nullptr && membership->over_capacity &&
              membership->member_count == 7 &&
              membership->configured_member_count == 8,
          "canonical Group runtime must remain bounded to seven members");
  require(group->participants_expected == 7,
          "Group coordinator accounting must use the bounded membership");

  for (std::size_t index = 1; index <= 6; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    char boot_id[32]{};
    std::snprintf(boot_id, sizeof(boot_id), "boot-%02u",
                  static_cast<unsigned>(index));
    require(coordinator.accept_heartbeat(
                participants, readiness, &configuration,
                heartbeat(participant_id, "group-main", boot_id, 1),
                1000) == GroupHeartbeatResult::OK,
            "an admitted Group member heartbeat should pass");
  }
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration,
              heartbeat("namo-node-07", "group-main", "boot-07", 1),
              1000) == GroupHeartbeatResult::GROUP_MISMATCH,
          "the eighth Group member heartbeat must fail closed");
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(only_group(coordinator)->participants_online == 7,
          "excluded input must not affect bounded Group health");
}

void test_canonical_roster_converges_both_group_views() {
  const auto owner = identity("namo-node-owner");
  const auto follower = identity("namo-node-follower");
  ParticipantRegistry owner_participants{};
  ParticipantReadinessRegistry owner_readiness{};
  add_active(&owner_participants, &owner_readiness, follower.node_id,
             "group-main", ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);

  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "canonical Group fixture bootstrap failed");
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, owner_participants, base,
                                   &configuration) ==
              SiteConfigurationResult::OK,
          "canonical Group fixture commit failed");

  ParticipantRegistry follower_participants{};
  ParticipantReadinessRegistry follower_readiness{};
  add_active(&follower_participants, &follower_readiness, owner.node_id,
             "group-main", ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);

  GroupCoordinator owner_coordinator{};
  GroupCoordinator follower_coordinator{};
  owner_coordinator.update(owner, owner_participants, owner_readiness,
                           &configuration, 1000);
  follower_coordinator.update(follower, follower_participants,
                              follower_readiness, &configuration, 1000);
  require(only_group(follower_coordinator)->leader_node_id[0] == '\0' &&
              !only_group(follower_coordinator)->local_leader,
          "an ineligible follower must not become Group Leader by default");

  require(owner_coordinator.accept_heartbeat(
              owner_participants, owner_readiness, &configuration,
              heartbeat(follower.node_id, "group-main", "boot-follower", 1),
              1000) == GroupHeartbeatResult::OK,
          "an active follower heartbeat must contribute Group health");
  require(follower_coordinator.accept_heartbeat(
              follower_participants, follower_readiness, &configuration,
              heartbeat(owner.node_id, "group-main", "boot-owner", 1),
              1000) == GroupHeartbeatResult::OK,
          "canonical Group owner must be admitted despite pair-local trust");
  owner_coordinator.update(owner, owner_participants, owner_readiness,
                           &configuration, 1000);
  follower_coordinator.update(follower, follower_participants,
                              follower_readiness, &configuration, 1000);
  require(follower_coordinator.accept_heartbeat(
              follower_participants, follower_readiness, &configuration,
              heartbeat(owner.node_id, "group-main", "boot-owner", 2,
                        only_group(owner_coordinator)->term),
              1100) == GroupHeartbeatResult::OK,
          "the elected Group winner term was not propagated to its follower");
  follower_coordinator.update(follower, follower_participants,
                              follower_readiness, &configuration, 1100);

  const auto *owner_group = only_group(owner_coordinator);
  const auto *follower_group = only_group(follower_coordinator);
  require(owner_group->participants_online == 2 &&
              follower_group->participants_online == 2,
          "both Group views must count the canonical roster online");
  require(owner_group->quorum_available && follower_group->quorum_available,
          "both Group views must observe canonical quorum");
  require(owner_group->local_leader && !follower_group->local_leader,
          "only the canonical owner view may be local Group Leader");
  require(std::strcmp(owner_group->leader_node_id, owner.node_id) == 0 &&
              std::strcmp(follower_group->leader_node_id, owner.node_id) == 0,
          "both Group views must elect the canonical owner");
  require(owner_group->term == follower_group->term,
          "both Group views must converge on one authority term");
  const uint32_t authority_term = owner_group->term;
  require(owner_coordinator.accept_heartbeat(
              owner_participants, owner_readiness, &configuration,
              heartbeat(follower.node_id, "group-main", "boot-follower", 2,
                        owner_group->term + 1),
              1100) == GroupHeartbeatResult::OK &&
              owner_group->term == authority_term,
          "an ineligible follower may contribute liveness but must not advance the Group term");
}

void test_indirect_canonical_group_member_can_lead() {
  const auto owner = identity("namo-node-owner");
  const auto local = identity("namo-node-z-local");
  const auto indirect = identity("namo-node-a-indirect");

  ParticipantRegistry owner_participants{};
  ParticipantReadinessRegistry owner_readiness{};
  add_active(&owner_participants, &owner_readiness, local.node_id,
             "group-main");
  add_active(&owner_participants, &owner_readiness, indirect.node_id,
             "group-main");
  auto configuration = canonical_configuration(owner, owner_participants);
  assign_canonical_group(&configuration, "group-main");

  ParticipantRegistry local_participants{};
  ParticipantReadinessRegistry local_readiness{};
  add_active(&local_participants, &local_readiness, owner.node_id,
             "group-main");
  require(local_participants.find(indirect.node_id) == nullptr &&
              local_readiness.find(indirect.node_id) == nullptr,
          "indirect Group fixture must not create a direct pairing record");

  GroupCoordinator coordinator{};
  coordinator.update(local, local_participants, local_readiness,
                     &configuration, owner.node_id, 1000);
  require(coordinator.accept_heartbeat(
              local_participants, local_readiness, &configuration,
              heartbeat(indirect.node_id, "group-main", "boot-indirect", 1),
              1000) == GroupHeartbeatResult::OK,
          "an active canonical Group member must not require a direct pair record");
  coordinator.update(local, local_participants, local_readiness,
                     &configuration, owner.node_id, 1000);

  const auto *group = only_group(coordinator);
  require(!group->local_leader &&
              std::strcmp(group->leader_node_id, indirect.node_id) == 0 &&
              group->participants_online == 2,
          "an indirect canonical candidate must participate in Group election");

  add_active(&local_participants, &local_readiness, indirect.node_id,
             "group-main");
  require(local_participants.revoke(indirect.node_id) ==
              ParticipantRegistryResult::OK,
          "direct Group revocation fixture failed");
  require(coordinator.accept_heartbeat(
              local_participants, local_readiness, &configuration,
              heartbeat(indirect.node_id, "group-main", "boot-indirect", 2,
                        group->term),
              1100) == GroupHeartbeatResult::PARTICIPANT_INACTIVE,
          "an explicit direct revocation must override canonical Group membership");
}

void test_group_recovers_an_expired_higher_term() {
  const auto owner = identity("namo-node-owner");
  const auto follower = identity("namo-node-follower");
  ParticipantRegistry owner_view{};
  ParticipantReadinessRegistry owner_readiness{};
  add_active(&owner_view, &owner_readiness, follower.node_id, "group-main",
             ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);

  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "Group term recovery fixture bootstrap failed");
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, owner_view, base, &configuration) ==
              SiteConfigurationResult::OK,
          "Group term recovery canonical roster failed");
  SiteConfigurationSnapshot previous_configuration = configuration;
  bool follower_found = false;
  for (auto &member : previous_configuration.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, follower.node_id) != 0)
      continue;
    member.leader_eligible = true;
    follower_found = true;
    break;
  }
  require(follower_found, "Group term recovery follower fixture is missing");

  ParticipantRegistry follower_view{};
  ParticipantReadinessRegistry follower_readiness{};
  add_active(&follower_view, &follower_readiness, owner.node_id, "group-main",
             ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  GroupCoordinator coordinator{};
  coordinator.update(follower, follower_view, follower_readiness,
                     &previous_configuration, 1000);
  require(!only_group(coordinator)->local_leader &&
              only_group(coordinator)->leader_node_id[0] == '\0' &&
              only_group(coordinator)->term == 1 &&
              !only_group(coordinator)->quorum_available,
          "an isolated Group follower must wait for canonical quorum");
  require(coordinator.accept_heartbeat(
              follower_view, follower_readiness, &previous_configuration,
              heartbeat(owner.node_id, "group-main", "boot-owner-old", 1, 3),
              1000) == GroupHeartbeatResult::OK,
          "legacy Group higher term fixture must be admitted");
  coordinator.update(follower, follower_view, follower_readiness,
                     &previous_configuration, 1000);
  require(only_group(coordinator)->local_leader &&
              only_group(coordinator)->term == 4,
          "legacy Group fixture must fence the observed higher term");

  coordinator.update(follower, follower_view, follower_readiness,
                     &configuration, 1001);
  require(!only_group(coordinator)->local_leader &&
              only_group(coordinator)->leader_node_id[0] == '\0' &&
              only_group(coordinator)->term == 4,
          "a replacement Group winner must issue its own higher term");
  require(coordinator.accept_heartbeat(
              follower_view, follower_readiness, &configuration,
              heartbeat(owner.node_id, "group-main", "boot-owner-old", 2, 5),
              1002) == GroupHeartbeatResult::OK,
          "replacement Group winner term must be admitted");
  coordinator.update(follower, follower_view, follower_readiness,
                     &configuration, 1002);
  require(!only_group(coordinator)->local_leader &&
              std::strcmp(only_group(coordinator)->leader_node_id,
                          owner.node_id) == 0 &&
              only_group(coordinator)->term == 5,
          "canonical Group winner did not establish its issued term");
  coordinator.update(follower, follower_view, follower_readiness,
                     &configuration, 7002);
  require(only_group(coordinator)->leader_node_id[0] == '\0' &&
              only_group(coordinator)->term == 5,
          "expired Group authority must leave no Leader");

  require(coordinator.accept_heartbeat(
              follower_view, follower_readiness, &configuration,
              heartbeat(owner.node_id, "group-main", "boot-owner-new", 1, 6),
              7003) == GroupHeartbeatResult::OK,
          "an eligible restarted Group owner must recover the expired term");
  coordinator.update(follower, follower_view, follower_readiness,
                     &configuration, 7003);
  require(!only_group(coordinator)->local_leader &&
              std::strcmp(only_group(coordinator)->leader_node_id,
                          owner.node_id) == 0 &&
              only_group(coordinator)->term == 6 &&
              only_group(coordinator)->participants_online == 2,
          "the Group follower must converge on a monotonic term after owner restart");
}

void test_directory_assignment_fences_local_election() {
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-remote", "group-main");
  auto configuration = canonical_configuration(local, participants);
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile.committed = true;
    member.spatial_profile.pose_version = 1;
    member.spatial_profile.calibration_version = 1;
    member.spatial_profile.transform.valid = true;
    member.spatial_profile.transform.sample_count = 1;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
  }
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, local.site_id);
  directory.generation = 1;
  directory.topology_revision = configuration.roster_revision;
  directory.committed = true;
  auto &entry = directory.groups[0];
  entry.occupied = true;
  std::strcpy(entry.group_id, "group-main");
  std::strcpy(entry.floorplan_id, "floor-main");
  std::strcpy(entry.leader_node_id, "namo-node-remote");
  entry.assignment_revision = 7;
  entry.member_count = 2;
  ParticipantCredentialRegistry credentials{};
  std::array<uint8_t, 32> key{};
  key.fill(7);
  require(credentials.set("namo-node-remote", 77, 1, key.data(), true) ==
              ParticipantCredentialResult::OK,
          "canonical leader credential fixture failed");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     &credentials, &directory, local.node_id, 1000);
  RoleElectionView assignment_view{};
  assignment_view.add(local.node_id, configuration.roster_revision, true);
  assignment_view.add("namo-node-remote", configuration.roster_revision,
                      true);
  auto remote =
      heartbeat("namo-node-remote", "group-main", "boot-remote", 1, 3);
  remote.election_view_digest =
      assignment_view.winner_contract_digest("group-main",
                                             "namo-node-remote");
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote,
              1000) == GroupHeartbeatResult::OK,
          "canonical leader heartbeat should be admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     &credentials, &directory, local.node_id, 1000);
  const auto *group = only_group(coordinator);
  require(!group->local_leader && group->canonical_assignment &&
              group->credentials_complete &&
              group->assignment_revision == 7 &&
              std::strcmp(group->leader_node_id, "namo-node-remote") == 0,
          "the persisted Directory assignment must fence local election");

  credentials.clear();
  coordinator.update(local, participants, readiness, &configuration,
                     &credentials, &directory, local.node_id, 1100);
  group = only_group(coordinator);
  require(!group->canonical_assignment && !group->credentials_complete,
          "a missing assigned-leader credential must fail closed");
}

void test_roster_expansion_bootstrap_preserves_directory_incumbent() {
  const auto local = identity("namo-node-10-fallback");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  const char *remote_ids[] = {
      "namo-node-20-site", "namo-node-30-incumbent",
      "namo-node-40-owner", "namo-node-50", "namo-node-60",
  };
  for (const char *participant_id : remote_ids)
    add_active(&participants, &readiness, participant_id, "group-main");
  auto configuration = canonical_configuration(local, participants);
  std::strcpy(configuration.configuration_owner_node_id,
              "namo-node-40-owner");
  assign_canonical_group(&configuration, "group-main");

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, local.site_id);
  directory.generation = 1;
  directory.topology_revision = configuration.roster_revision - 1;
  directory.committed = true;
  auto &entry = directory.groups[0];
  entry.occupied = true;
  std::strcpy(entry.group_id, "group-main");
  std::strcpy(entry.floorplan_id, "floor-main");
  std::strcpy(entry.leader_node_id, "namo-node-30-incumbent");
  entry.assignment_revision = 30;
  entry.member_count = 5;

  ParticipantCredentialRegistry credentials{};
  std::array<uint8_t, 32> key{};
  key.fill(0x30);
  uint64_t token = 300;
  for (const char *participant_id : remote_ids) {
    require(credentials.set(participant_id, ++token, 1, key.data(), true) ==
                ParticipantCredentialResult::OK,
            "the expansion bootstrap credential fixture failed");
  }

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     &credentials, &directory, "namo-node-20-site", 1000);
  auto incumbent = heartbeat("namo-node-30-incumbent", "group-main",
                             "boot-incumbent", 1, 38);
  incumbent.election_view_digest = 1;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, incumbent, 1000) ==
              GroupHeartbeatResult::OK,
          "the persisted incumbent heartbeat should be admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     &credentials, &directory, "namo-node-20-site", 1000);

  RoleElectionView contract{};
  for (const auto &member : configuration.members) {
    if (member.occupied)
      contract.add(member.participant_id, configuration.roster_revision,
                   true);
  }
  const auto *group = only_group(coordinator);
  require(group->assignment_revision == 0 &&
              !group->canonical_assignment &&
              group->election_view_digest ==
                  contract.winner_contract_digest(
                      "group-main", "namo-node-30-incumbent"),
          "a restarted 5-to-6 transition must elect the valid Directory incumbent before canonical promotion");
}

void test_divergent_directory_assignment_fails_closed_until_majority_agrees() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  add_active(&participants, &readiness, "namo-node-b", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, local.site_id);
  directory.committed = true;
  auto &entry = directory.groups[0];
  entry.occupied = true;
  std::strcpy(entry.group_id, "group-main");
  std::strcpy(entry.floorplan_id, "floor-main");
  std::strcpy(entry.leader_node_id, "namo-node-a");
  entry.assignment_revision = 7;
  entry.member_count = 3;

  RoleElectionView unfenced_view{};
  unfenced_view.add(local.node_id, 0, true);
  unfenced_view.add("namo-node-a", 0, true);
  unfenced_view.add("namo-node-b", 0, true);
  const uint32_t divergent_digest =
      unfenced_view.contract_digest("group-main");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &directory, local.node_id, 1000);
  auto a = heartbeat("namo-node-a", "group-main", "boot-a", 1);
  auto b = heartbeat("namo-node-b", "group-main", "boot-b", 1);
  a.election_view_digest = divergent_digest;
  b.election_view_digest = divergent_digest;
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       a, 1000) ==
                  GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, b, 1000) ==
                  GroupHeartbeatResult::OK,
          "divergent assignment heartbeats were not admitted for fencing");
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &directory, local.node_id, 1000);
  const auto *group = only_group(coordinator);
  require(!group->quorum_available && group->leader_node_id[0] == '\0' &&
              !group->canonical_assignment,
          "a divergent Directory assignment opened Group authority");

  RoleElectionView canonical_view{};
  const uint64_t qualifier = configuration.roster_revision;
  canonical_view.add(local.node_id, qualifier, true);
  canonical_view.add("namo-node-a", qualifier, true);
  canonical_view.add("namo-node-b", qualifier, true);
  const uint32_t canonical_digest =
      canonical_view.winner_contract_digest("group-main", "namo-node-a");
  a.sequence = 2;
  b.sequence = 2;
  a.election_view_digest = canonical_digest;
  b.election_view_digest = canonical_digest;
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       a, 1100) ==
                  GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, b, 1100) ==
                  GroupHeartbeatResult::OK,
          "canonical assignment heartbeats were not admitted");
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &directory, local.node_id, 1100);
  group = only_group(coordinator);
  require(group->quorum_available && group->canonical_assignment &&
              group->election_endorsements == 3 &&
              std::strcmp(group->leader_node_id, "namo-node-a") == 0,
          "a majority-endorsed Directory assignment did not restore authority");
  const uint32_t stable_term = group->term;

  a.sequence = 3;
  b.sequence = 3;
  a.term = stable_term;
  b.term = stable_term;
  a.election_view_digest = divergent_digest;
  b.election_view_digest = divergent_digest;
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       a, 1200) ==
                  GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, b, 1200) ==
                  GroupHeartbeatResult::OK,
          "temporary divergent views were not admitted for fail-closed testing");
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &directory, local.node_id, 1200);
  group = only_group(coordinator);
  require(!group->quorum_available && group->leader_node_id[0] == '\0',
          "a temporary divergent view did not suspend Group authority");

  a.sequence = 4;
  b.sequence = 4;
  a.election_view_digest = canonical_digest;
  b.election_view_digest = canonical_digest;
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       a, 1300) ==
                  GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, b, 1300) ==
                  GroupHeartbeatResult::OK,
          "matching views were not admitted after a transient fence gap");
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &directory, local.node_id, 1300);
  group = only_group(coordinator);
  require(group->quorum_available && group->canonical_assignment &&
              group->term == stable_term &&
              std::strcmp(group->leader_node_id, "namo-node-a") == 0,
          "the same non-rebooted Group Leader did not resume its suspended term");
}

void test_returning_member_preserves_established_group_leader_during_view_rollout() {
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    ParticipantReadinessRegistry readiness{};
    std::array<std::array<char, 32>, 6> node_ids{};
    std::array<std::array<char, 32>, 6> boot_ids{};
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      std::snprintf(node_ids[index].data(), node_ids[index].size(),
                    "namo-node-%02u", static_cast<unsigned>(index));
      std::snprintf(boot_ids[index].data(), boot_ids[index].size(),
                    "boot-%02u", static_cast<unsigned>(index));
      add_active(&participants, &readiness, node_ids[index].data(),
                 "group-main");
    }
    auto configuration = canonical_configuration(local, participants);
    assign_canonical_group(&configuration, "group-main");

    SiteDirectorySnapshot directory{};
    std::strcpy(directory.site_id, local.site_id);
    directory.committed = true;
    auto &entry = directory.groups[0];
    entry.occupied = true;
    std::strcpy(entry.group_id, "group-main");
    std::strcpy(entry.floorplan_id, "floor-main");
    std::strcpy(entry.leader_node_id, node_ids[0].data());
    entry.assignment_revision = 9;
    entry.member_count = static_cast<uint8_t>(node_count);

    ParticipantCredentialRegistry credentials{};
    std::array<uint8_t, 32> key{};
    key.fill(9);
    uint64_t token = 100;
    for (std::size_t index = 0; index + 1 < node_count; ++index)
      require(credentials.set(node_ids[index].data(), token++, 1, key.data(),
                              true) == ParticipantCredentialResult::OK,
              "returning Group member credential fixture failed");

    const uint64_t qualifier = configuration.roster_revision;
    RoleElectionView canonical_view{};
    canonical_view.add(local.node_id, qualifier, true);
    for (std::size_t index = 0; index + 1 < node_count; ++index)
      canonical_view.add(node_ids[index].data(), qualifier, true);
    const uint32_t canonical_digest = canonical_view.winner_contract_digest(
        "group-main", node_ids[0].data());

    GroupCoordinator coordinator{};
    const char *site_leader = node_count > 2 ? node_ids[1].data()
                                              : local.node_id;
    coordinator.update(local, participants, readiness, &configuration,
                       &credentials, &directory, site_leader, 1000);
    const std::size_t remote_count = node_count - 1;
    const std::size_t survivor_remote_count =
        strict_majority(static_cast<uint8_t>(node_count)) - 1U;
    for (std::size_t index = 0; index < survivor_remote_count; ++index) {
      auto peer = heartbeat(node_ids[index].data(), "group-main",
                            boot_ids[index].data(), 1);
      peer.election_view_digest = canonical_digest;
      require(coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, peer, 1000) ==
                  GroupHeartbeatResult::OK,
              "canonical Group heartbeat was rejected before member return");
    }
    coordinator.update(local, participants, readiness, &configuration,
                       &credentials, &directory, site_leader, 1000);
    const auto *group = only_group(coordinator);
    require(group->quorum_available && group->canonical_assignment &&
                group->credentials_complete &&
                std::strcmp(group->leader_node_id, node_ids[0].data()) == 0,
            "surviving Group quorum did not establish its canonical leader");
    const uint32_t stable_term = group->term;

    uint32_t now_ms = 1100;
    for (std::size_t index = survivor_remote_count; index < remote_count;
         ++index, now_ms += 100) {
      auto returning = heartbeat(node_ids[index].data(), "group-main",
                                 boot_ids[index].data(), 1, stable_term);
      returning.election_view_digest =
          canonical_digest ^ static_cast<uint32_t>(0x55AA5500U + index);
      require(coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, returning,
                                           now_ms) ==
                  GroupHeartbeatResult::OK,
              "returning Group member heartbeat was rejected");
      coordinator.update(local, participants, readiness, &configuration,
                         &credentials, &directory, site_leader, now_ms);
      group = only_group(coordinator);
      require(group->participants_online == index + 2 &&
                  group->quorum_available && group->canonical_assignment &&
                  group->credentials_complete && group->term == stable_term &&
                  std::strcmp(group->leader_node_id, node_ids[0].data()) == 0,
              "stale returning members suspended the established Group Leader");
    }

    for (std::size_t index = survivor_remote_count; index < remote_count;
         ++index, now_ms += 100) {
      auto converged = heartbeat(node_ids[index].data(), "group-main",
                                 boot_ids[index].data(), 2, stable_term);
      converged.election_view_digest = canonical_digest;
      require(coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, converged,
                                           now_ms) ==
                  GroupHeartbeatResult::OK,
              "returning Group member did not adopt the canonical contract");
      coordinator.update(local, participants, readiness, &configuration,
                         &credentials, &directory, site_leader, now_ms);
      group = only_group(coordinator);
      require(group->quorum_available && group->canonical_assignment &&
                  group->credentials_complete && group->term == stable_term &&
                  std::strcmp(group->leader_node_id, node_ids[0].data()) == 0,
              "Group Leader changed while returning members converged");
    }
  }
}

void test_mixed_v11_v12_group_contracts_converge_through_seven_nodes() {
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    ParticipantReadinessRegistry readiness{};
    std::array<std::array<char, 32>, 6> node_ids{};
    std::array<std::array<char, 32>, 6> boot_ids{};
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      std::snprintf(node_ids[index].data(), node_ids[index].size(),
                    "namo-node-%02u", static_cast<unsigned>(index));
      std::snprintf(boot_ids[index].data(), boot_ids[index].size(),
                    "boot-%02u", static_cast<unsigned>(index));
      add_active(&participants, &readiness, node_ids[index].data(),
                 "group-main");
    }
    auto configuration = canonical_configuration(local, participants);
    assign_canonical_group(&configuration, "group-main");

    SiteDirectorySnapshot directory{};
    std::strcpy(directory.site_id, local.site_id);
    directory.committed = true;
    auto &entry = directory.groups[0];
    entry.occupied = true;
    std::strcpy(entry.group_id, "group-main");
    std::strcpy(entry.floorplan_id, "floor-main");
    std::strcpy(entry.leader_node_id, node_ids[0].data());
    entry.assignment_revision = 12;
    entry.member_count = static_cast<uint8_t>(node_count);

    ParticipantCredentialRegistry credentials{};
    std::array<uint8_t, 32> key{};
    key.fill(12);
    for (std::size_t index = 0; index + 1 < node_count; ++index)
      require(credentials.set(node_ids[index].data(), 200 + index, 1,
                              key.data(), true) ==
                  ParticipantCredentialResult::OK,
              "mixed-version Group credential fixture failed");

    RoleElectionView current_view{};
    RoleElectionView v11_view{};
    const uint64_t assignment_qualifier =
        (static_cast<uint64_t>(entry.assignment_revision) << 32U) |
        group_directory_assignment_hash(entry);
    current_view.add(local.node_id, configuration.roster_revision, true);
    v11_view.add(local.node_id, assignment_qualifier, true);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      current_view.add(node_ids[index].data(), configuration.roster_revision,
                       true);
      v11_view.add(node_ids[index].data(), assignment_qualifier, true);
    }
    const uint32_t current_digest = current_view.winner_contract_digest(
        "group-main", node_ids[0].data());
    const uint32_t v11_digest = v11_view.winner_contract_digest(
        "group-main", node_ids[0].data());
    const char *site_leader = node_count > 2 ? node_ids[1].data()
                                              : local.node_id;

    GroupCoordinator coordinator{};
    coordinator.update(local, participants, readiness, &configuration,
                       &credentials, &directory, site_leader, 1000);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      auto peer = heartbeat(node_ids[index].data(), "group-main",
                            boot_ids[index].data(), 1);
      peer.election_contract_version = index % 2 == 0 ? 2 : 3;
      peer.election_view_digest = peer.election_contract_version == 2
                                      ? v11_digest
                                      : current_digest;
      require(coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, peer, 1000) ==
                  GroupHeartbeatResult::OK,
              "mixed-version Group heartbeat was rejected");
    }
    coordinator.update(local, participants, readiness, &configuration,
                       &credentials, &directory, site_leader, 1000);
    const auto *group = only_group(coordinator);
    require(group->participants_online == node_count &&
                group->quorum_available && group->canonical_assignment &&
                group->credentials_complete &&
                group->election_endorsements == node_count &&
                std::strcmp(group->leader_node_id, node_ids[0].data()) == 0,
            "mixed v11/v12 Group contracts failed to converge at 3-7 Nodes");
  }
}

void test_zero_election_digest_does_not_disable_group_fencing() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  add_active(&participants, &readiness, "namo-node-b", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");

  RoleElectionView view{};
  view.add(local.node_id, 0, true);
  view.add("namo-node-a", 0, true);
  view.add("namo-node-b", 0, true);
  const uint32_t shared_digest = view.contract_digest("group-main");
  auto a = heartbeat("namo-node-a", "group-main", "boot-a", 1);
  auto b = heartbeat("namo-node-b", "group-main", "boot-b", 1);
  a.election_view_digest = 0;
  b.election_view_digest = shared_digest ^ 1U;

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       a, 1000) ==
                  GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(participants, readiness,
                                           &configuration, b, 1000) ==
                  GroupHeartbeatResult::OK,
          "mixed-version Group heartbeats were rejected before fencing");
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  const auto *group = only_group(coordinator);
  require(!group->quorum_available && group->leader_node_id[0] == '\0' &&
              group->election_fenced,
          "a zero election digest disabled the Group election fence");
}

void test_stale_persisted_roster_falls_back_to_canonical_membership() {
  const auto local = identity("namo-node-00");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-01", "group-main");
  add_active(&participants, &readiness, "namo-node-02", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");
  configuration.roster_revision = 10;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floor-main");
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 101;
  configuration.floorplan.image_hash = 202;

  auto stale_configuration = configuration;
  stale_configuration.roster_revision = 9;
  for (auto &member : stale_configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, local.node_id) != 0)
      member = SiteConfigurationMember{};
  }
  GroupRosterSnapshot stale_roster{};
  require(prepare_group_roster_from_site_configuration(
              stale_configuration, "group-main", local.node_id,
              &stale_roster) == GroupRosterResult::OK &&
              stale_roster.member_count() == 1,
          "stale one-member roster fixture failed");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     &stale_roster, nullptr, nullptr, local.node_id, 1000);
  require(coordinator.local_membership() != nullptr &&
              coordinator.local_membership()->member_count == 3 &&
              only_group(coordinator)->participants_expected == 3,
          "stale persisted roster overrode the canonical three-member Group");
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration,
              heartbeat("namo-node-01", "group-main", "boot-01", 1),
              1000) == GroupHeartbeatResult::OK &&
              coordinator.accept_heartbeat(
                  participants, readiness, &configuration,
                  heartbeat("namo-node-02", "group-main", "boot-02", 1),
                  1000) == GroupHeartbeatResult::OK,
          "new canonical members were rejected by the stale roster fence");
  coordinator.update(local, participants, readiness, &configuration,
                     &stale_roster, nullptr, nullptr, local.node_id, 1000);
  require(only_group(coordinator)->participants_online == 3,
          "canonical members did not become online after roster expansion");
}

void test_credential_incomplete_remote_never_leads_group() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  auto peer = heartbeat("namo-node-a", "group-main", "boot-a", 1);
  peer.credentials_complete = false;
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       peer, 1000) == GroupHeartbeatResult::OK,
          "credential-incomplete Group heartbeat should remain online");
  coordinator.update(local, participants, readiness, &configuration,
                     local.node_id, 1000);
  const auto *group = only_group(coordinator);
  require(!group->local_leader && group->participants_online == 2,
          "a credential-incomplete remote forced a forbidden three-role fallback");
}

void test_credential_incomplete_local_candidate_yields_group_authority() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  ParticipantCredentialRegistry credentials{};
  add_active(&participants, &readiness, "namo-node-a", "group-main");
  auto configuration = canonical_configuration(local, participants);
  assign_canonical_group(&configuration, "group-main");

  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &credentials, nullptr, local.node_id, 1000);
  RoleElectionView view{};
  view.add(local.node_id, configuration.roster_revision, true);
  view.add("namo-node-a", configuration.roster_revision, true);
  auto peer = heartbeat("namo-node-a", "group-main", "boot-a", 1);
  peer.credentials_complete = true;
  peer.election_view_digest =
      view.winner_contract_digest("group-main", "namo-node-a");
  require(coordinator.accept_heartbeat(participants, readiness, &configuration,
                                       peer, 1000) ==
              GroupHeartbeatResult::OK,
          "credential-complete Group peer was not admitted");
  coordinator.update(local, participants, readiness, &configuration, nullptr,
                     &credentials, nullptr, local.node_id, 1000);
  const auto *group = only_group(coordinator);
  require(!group->local_leader && group->leader_node_id[0] == '\0' &&
              !group->quorum_available,
          "a Group without locally verifiable credentials retained authority");
}

void test_mixed_eligibility_election_converges_from_two_to_seven_nodes() {
  constexpr std::size_t max_nodes = GroupRosterSnapshot::MAX_MEMBERS;
  std::array<PlatformIdentity, max_nodes> nodes{};
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    char node_id[32]{};
    std::snprintf(node_id, sizeof(node_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    nodes[index] = identity(node_id);
  }

  for (std::size_t node_count = 2; node_count <= max_nodes; ++node_count) {
    const std::size_t leader_index = node_count > 2 ? 1 : 0;
    ParticipantRegistry owner_participants{};
    ParticipantReadinessRegistry owner_readiness{};
    for (std::size_t index = 1; index < node_count; ++index)
      add_active(&owner_participants, &owner_readiness, nodes[index].node_id,
                 "group-main");

    auto configuration =
        canonical_configuration(nodes[0], owner_participants);
    assign_canonical_group(&configuration, "group-main");
    configuration.floorplan.committed = true;
    std::strcpy(configuration.floorplan.floorplan_id, "floor-main");
    configuration.floorplan.geometry_version = 1;
    configuration.floorplan.config_hash = 101;
    configuration.floorplan.image_hash = 202;
    for (auto &member : configuration.members) {
      if (member.occupied)
        member.leader_eligible =
            std::strcmp(member.participant_id,
                        nodes[node_count - 1].node_id) != 0;
    }

    GroupRosterSnapshot roster{};
    require(prepare_group_roster_from_site_configuration(
                configuration, "group-main", nodes[leader_index].node_id,
                &roster) ==
                GroupRosterResult::OK,
            "mixed-eligibility Group roster fixture failed");

    SiteDirectorySnapshot directory{};
    std::strcpy(directory.site_id, configuration.site_id);
    directory.generation = 1;
    directory.topology_revision = configuration.roster_revision;
    directory.committed = true;
    auto &directory_group = directory.groups[0];
    directory_group.occupied = true;
    std::strcpy(directory_group.group_id, "group-main");
    std::strcpy(directory_group.floorplan_id, "floor-main");
    std::strcpy(directory_group.leader_node_id,
                nodes[leader_index].node_id);
    directory_group.assignment_revision = roster.assignment_revision;
    directory_group.member_count = static_cast<uint8_t>(node_count);

    RoleElectionView shared_view{};
    const uint64_t qualifier = configuration.roster_revision;
    for (std::size_t index = 0; index < node_count; ++index)
      shared_view.add(nodes[index].node_id, qualifier, true);
    const uint32_t shared_digest =
        shared_view.winner_contract_digest(
            "group-main", nodes[leader_index].node_id);

    std::array<GroupCoordinator, max_nodes> coordinators{};
    std::array<ParticipantRegistry, max_nodes> participant_views{};
    std::array<ParticipantReadinessRegistry, max_nodes> readiness_views{};
    std::array<ParticipantCredentialRegistry, max_nodes> credential_views{};
    std::array<uint8_t, 32> key{};
    key.fill(7);

    for (std::size_t local = 0; local < node_count; ++local) {
      for (std::size_t remote = 0; remote < node_count; ++remote) {
        if (remote == local)
          continue;
        add_active(&participant_views[local], &readiness_views[local],
                   nodes[remote].node_id, "group-main");
        require(credential_views[local].set(
                    nodes[remote].node_id,
                    static_cast<uint64_t>(100 + remote), 1, key.data(), true) ==
                    ParticipantCredentialResult::OK,
                "mixed-eligibility credential fixture failed");
      }
      coordinators[local].update(
          nodes[local], participant_views[local], readiness_views[local],
          &configuration, &roster, &credential_views[local], &directory,
          nodes[0].node_id, 1000);
    }

    for (std::size_t local = 0; local < node_count; ++local) {
      for (std::size_t remote = 0; remote < node_count; ++remote) {
        if (remote == local)
          continue;
        char boot_id[32]{};
        std::snprintf(boot_id, sizeof(boot_id), "boot-%02u",
                      static_cast<unsigned>(remote));
        auto peer = heartbeat(nodes[remote].node_id, "group-main", boot_id, 1);
        peer.credentials_complete = true;
        peer.election_view_digest = shared_digest;
        require(coordinators[local].accept_heartbeat(
                    participant_views[local], readiness_views[local],
                    &configuration, peer, 1000) == GroupHeartbeatResult::OK,
                "mixed-eligibility Group heartbeat was rejected");
      }
      coordinators[local].update(
          nodes[local], participant_views[local], readiness_views[local],
          &configuration, &roster, &credential_views[local], &directory,
          nodes[0].node_id, 1000);

      const auto *group = only_group(coordinators[local]);
      const bool converged =
          group->participants_expected == node_count &&
          group->participants_online == node_count &&
          group->quorum_available && group->canonical_assignment &&
          group->credentials_complete &&
          group->election_view_digest == shared_digest &&
          std::strcmp(group->leader_node_id,
                      nodes[leader_index].node_id) == 0;
      if (!converged) {
        std::fprintf(
            stderr,
            "mixed eligibility diagnostic nodes=%u local=%u expected=%u "
            "online=%u quorum=%u canonical=%u credentials=%u digest=%u/%u "
            "endorsements=%u leader=%s\n",
            static_cast<unsigned>(node_count), static_cast<unsigned>(local),
            static_cast<unsigned>(group->participants_expected),
            static_cast<unsigned>(group->participants_online),
            group->quorum_available ? 1U : 0U,
            group->canonical_assignment ? 1U : 0U,
            group->credentials_complete ? 1U : 0U,
            static_cast<unsigned>(group->election_view_digest),
            static_cast<unsigned>(shared_digest),
            static_cast<unsigned>(group->election_endorsements),
            group->leader_node_id);
      }
      require(converged,
              "mixed eligibility split an otherwise identical Group view");
      if (node_count == 2)
        require(std::strcmp(group->leader_node_id, nodes[0].node_id) == 0,
                "the sole eligible two-Node candidate did not take emergency Group authority");
      if (local + 1 == node_count)
        require(!group->local_leader,
                "an ineligible local Node became Group Leader");
    }

    if (node_count > 2) {
      const std::size_t changing_sender = node_count - 1;
      for (std::size_t local = 0; local < node_count; ++local) {
        if (local == changing_sender)
          continue;
        const auto *before = only_group(coordinators[local]);
        char leader_before[PlatformIdentity::MAX_ID_LENGTH + 1]{};
        std::strcpy(leader_before, before->leader_node_id);
        const uint32_t term_before = before->term;

        char boot_id[32]{};
        std::snprintf(boot_id, sizeof(boot_id), "boot-%02u",
                      static_cast<unsigned>(changing_sender));
        auto changing = heartbeat(nodes[changing_sender].node_id,
                                  "group-main", boot_id, 2);
        changing.credentials_complete = false;
        changing.election_view_digest = shared_digest;
        require(coordinators[local].accept_heartbeat(
                    participant_views[local], readiness_views[local],
                    &configuration, changing, 1100) == GroupHeartbeatResult::OK,
                "asynchronous credential heartbeat was rejected");
        coordinators[local].update(
            nodes[local], participant_views[local], readiness_views[local],
            &configuration, &roster, &credential_views[local], &directory,
            nodes[0].node_id, 1100);

        const auto *after = only_group(coordinators[local]);
        require(after->quorum_available &&
                    std::strcmp(after->leader_node_id, leader_before) == 0 &&
                    after->term == term_before &&
                    after->election_view_digest == shared_digest,
                "credential convergence changed a live Group Leader contract");
      }
    }
  }
}

}  // namespace

void run_group_coordinator_tests() {
  test_standalone_uses_the_same_authority_contract();
  test_standalone_slot_is_replaced_by_canonical_group();
  test_configured_slot_tracks_canonical_group_change();
  test_configured_slot_returns_to_standalone();
  test_standalone_slot_accepts_seven_member_canonical_group();
  test_eligible_remote_leader_and_lease_expiry();
  test_sequence_and_boot_rules();
  test_leader_eligibility_is_explicit();
  test_satellite_can_be_online_but_never_lead();
  test_admission_rejects_wrong_or_stale_input();
  test_multiple_remote_groups_without_local_assignment_fail_closed();
  test_group_leader_avoids_site_and_configuration_roles();
  test_configuration_owner_leads_before_site_leader_fallback();
  test_site_leader_can_lead_when_no_other_candidate_exists();
  test_legacy_twenty_node_group_is_bounded_to_seven();
  test_ambiguous_local_multi_group_assignment_fails_closed();
  test_canonical_local_assignment_selects_only_its_group();
  test_canonical_group_capacity_excludes_the_eighth_member();
  test_canonical_roster_converges_both_group_views();
  test_indirect_canonical_group_member_can_lead();
  test_group_recovers_an_expired_higher_term();
  test_directory_assignment_fences_local_election();
  test_roster_expansion_bootstrap_preserves_directory_incumbent();
  test_divergent_directory_assignment_fails_closed_until_majority_agrees();
  test_returning_member_preserves_established_group_leader_during_view_rollout();
  test_mixed_v11_v12_group_contracts_converge_through_seven_nodes();
  test_zero_election_digest_does_not_disable_group_fencing();
  test_stale_persisted_roster_falls_back_to_canonical_membership();
  test_credential_incomplete_remote_never_leads_group();
  test_credential_incomplete_local_candidate_yields_group_authority();
  test_mixed_eligibility_election_converges_from_two_to_seven_nodes();
}
