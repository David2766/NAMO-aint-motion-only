#include "site_coordinator.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_coordinator_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity identity(const char *node_id) {
  PlatformIdentity value{};
  std::strcpy(value.site_id, "site-test");
  std::strcpy(value.node_id, node_id);
  return value;
}

void add_active(ParticipantRegistry *participants, const char *participant_id,
                ParticipantType type = ParticipantType::NAMO_NODE,
                ParticipantTrust trust = ParticipantTrust::OFFICIAL_VERIFIED) {
  require(participants->discover(participant_id, type) ==
              ParticipantRegistryResult::OK,
          "participant discovery failed");
  require(participants->classify(participant_id, trust) ==
              ParticipantRegistryResult::OK,
          "participant classification failed");
  require(participants->activate(participant_id) ==
              ParticipantRegistryResult::OK,
          "participant activation failed");
}

SiteLeaderHeartbeat heartbeat(const char *participant_id, const char *boot_id,
                              uint32_t sequence, uint32_t term = 1,
                              uint32_t lease_ms = 6000,
                              uint32_t sample_age_ms = 0,
                              uint32_t directory_revision = 0,
                              uint32_t directory_digest = 0) {
  SiteLeaderHeartbeat value{};
  value.participant_id = participant_id;
  value.boot_id = boot_id;
  value.sequence = sequence;
  value.term = term;
  value.lease_ms = lease_ms;
  value.sample_age_ms = sample_age_ms;
  value.directory_revision = directory_revision;
  value.directory_digest = directory_digest;
  return value;
}

SiteConfigurationSnapshot canonical_configuration(
    const PlatformIdentity &owner, const ParticipantRegistry &participants);

void minority_higher_directory_fork_cannot_take_site_leadership() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  add_active(&participants, "namo-node-b");
  auto configuration = canonical_configuration(local, participants);

  SiteRoleElectionView canonical_view{};
  SiteRoleElectionView fork_view{};
  for (const char *node : {local.node_id, "namo-node-a", "namo-node-b"}) {
    canonical_view.add(node, configuration.roster_revision, true);
    fork_view.add(node, configuration.roster_revision, true);
  }
  const uint32_t canonical_digest =
      canonical_view.winner_contract_digest("site", "namo-node-a");
  const uint32_t fork_digest =
      fork_view.winner_contract_digest("site", "namo-node-b");

  SiteCoordinator coordinator{};
  coordinator.set_local_directory_fence(5, 0x11111111);
  auto a = heartbeat("namo-node-a", "boot-a", 1, 1, 6000, 0, 5,
                     0x11111111);
  auto b = heartbeat("namo-node-b", "boot-b", 1, 1, 6000, 0, 6,
                     0x22222222);
  a.election_view_digest = canonical_digest;
  b.election_view_digest = fork_digest;
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1000) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
          "Directory fork heartbeats were not admitted for election");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(coordinator.state().quorum_available &&
              !coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-a") == 0,
          "a minority-only higher Directory fork took Site leadership");
}

void minority_higher_directory_forks_are_rejected_through_seven_nodes() {
  constexpr std::size_t max_remote = 6;
  for (std::size_t node_count = 4; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    std::array<std::array<char, 32>, max_remote> node_ids{};
    std::array<std::array<char, 32>, max_remote> boot_ids{};
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      std::snprintf(node_ids[index].data(), node_ids[index].size(),
                    "namo-node-%02u", static_cast<unsigned>(index));
      std::snprintf(boot_ids[index].data(), boot_ids[index].size(),
                    "boot-%02u", static_cast<unsigned>(index));
      add_active(&participants, node_ids[index].data());
    }
    const auto configuration = canonical_configuration(local, participants);
    const std::size_t canonical_remote_count =
        strict_majority(static_cast<uint8_t>(node_count)) - 1U;

    SiteRoleElectionView canonical_view{};
    SiteRoleElectionView fork_view{};
    canonical_view.add(local.node_id, configuration.roster_revision, true);
    fork_view.add(local.node_id, configuration.roster_revision, true);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      canonical_view.add(node_ids[index].data(),
                         configuration.roster_revision, true);
      fork_view.add(node_ids[index].data(),
                    configuration.roster_revision, true);
    }
    const uint32_t canonical_digest = canonical_view.winner_contract_digest(
        "site", node_ids[0].data());
    const uint32_t fork_digest = fork_view.winner_contract_digest(
        "site", node_ids[canonical_remote_count].data());

    SiteCoordinator coordinator{};
    coordinator.set_local_directory_fence(5, 0x11111111);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      const bool canonical = index < canonical_remote_count;
      auto peer = heartbeat(node_ids[index].data(), boot_ids[index].data(), 1,
                            1, 6000, 0, canonical ? 5 : 6,
                            canonical ? 0x11111111 : 0x22222222);
      peer.election_view_digest = canonical ? canonical_digest : fork_digest;
      require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
              "multi-node Directory fork heartbeat was rejected");
    }
    coordinator.update(local, participants, &configuration, true, 1000);
    require(coordinator.state().quorum_available &&
                std::strcmp(coordinator.state().leader_node_id,
                            node_ids[0].data()) == 0,
            "a minority higher Directory fork won a 4-7 Node election");
  }
}

void current_site_leader_can_publish_a_new_directory_before_replica_quorum() {
  const auto local = identity("namo-node-a");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-b");
  add_active(&participants, "namo-node-c");
  auto configuration = canonical_configuration(local, participants);
  for (auto &member : configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, local.node_id) != 0)
      member.leader_eligible = false;
  }

  SiteCoordinator coordinator{};
  coordinator.set_local_directory_fence(5, 0x11111111);
  RoleElectionView initial_view{};
  for (const char *node : {local.node_id, "namo-node-b", "namo-node-c"})
    initial_view.add(node, configuration.roster_revision, true);
  const uint32_t initial_digest =
      initial_view.winner_contract_digest("site", local.node_id);
  auto b = heartbeat("namo-node-b", "boot-b", 1, 1, 6000, 0, 5,
                     0x11111111);
  auto c = heartbeat("namo-node-c", "boot-c", 1, 1, 6000, 0, 5,
                     0x11111111);
  b.election_view_digest = initial_digest;
  c.election_view_digest = initial_digest;
  require(coordinator.accept_heartbeat(participants, &configuration, b, 1000) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, c,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
          "initial Site election fixtures were rejected");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(coordinator.state().local_leader,
          "fixture did not establish the local Site Leader");

  coordinator.set_local_directory_fence(6, 0x22222222);
  b.sequence = 2;
  c.sequence = 2;
  // Followers still endorse the immediately preceding canonical contract
  // until the new Directory has reached them. The incumbent transition fence
  // must bridge that bounded publication window.
  b.election_view_digest = initial_digest;
  c.election_view_digest = initial_digest;
  require(coordinator.accept_heartbeat(participants, &configuration, b, 1100) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, c,
                                           1100) ==
                  SiteHeartbeatAdmissionResult::OK,
          "Directory rollout heartbeats were rejected");
  coordinator.update(local, participants, &configuration, true, 1100);
  require(coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          local.node_id) == 0,
          "an active Site Leader lost authority before publishing its Directory");
}

void zero_election_digest_is_not_a_fence_bypass() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  add_active(&participants, "namo-node-b");
  const auto configuration = canonical_configuration(local, participants);

  RoleElectionView view{};
  for (const char *node : {local.node_id, "namo-node-a", "namo-node-b"})
    view.add(node, configuration.roster_revision, true);
  const uint32_t shared_digest =
      view.winner_contract_digest("site", "namo-node-a");
  auto a = heartbeat("namo-node-a", "boot-a", 1, 1, 6000, 0, 5,
                     0x11111111);
  auto b = heartbeat("namo-node-b", "boot-b", 1, 1, 6000, 0, 5,
                     0x11111111);
  a.election_view_digest = 0;
  b.election_view_digest = shared_digest ^ 1U;

  SiteCoordinator coordinator{};
  coordinator.set_local_directory_fence(5, 0x11111111);
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1000) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
          "mixed-version Site heartbeats were rejected before fencing");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(!coordinator.state().quorum_available &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().election_fenced,
          "a zero election digest disabled the Site election fence");
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

void standalone_node_uses_the_site_authority_contract() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  coordinator.update(identity("namo-node-local"), participants, 1000);
  const auto &state = coordinator.state();
  require(state.local_leader && state.term == 1 &&
              std::strcmp(state.leader_node_id, "namo-node-local") == 0,
          "a standalone node must hold the initial Site lease");
  require(state.participants_expected == 1 && state.participants_online == 1,
          "standalone counts must contain only the local Node");
  require(SiteCoordinator::lease_remaining_ms(state, 1000) ==
              SiteCoordinator::DEFAULT_LEASE_MS,
          "standalone authority must use the normal lease duration");
}

void standalone_follower_adopts_the_canonical_site_winner_at_equal_term() {
  constexpr uint32_t directory_revision = 5;
  constexpr uint32_t directory_digest = 0x51515151;
  const auto owner = identity("namo-node-owner");
  const auto follower = identity("namo-node-follower");

  SiteCoordinator coordinator{};
  ParticipantRegistry empty{};
  coordinator.update(follower, empty, 1000);
  require(coordinator.state().local_leader &&
              coordinator.state().term == 1 &&
              std::strcmp(coordinator.state().leader_node_id,
                          follower.node_id) == 0,
          "standalone follower fixture did not establish local authority");

  ParticipantRegistry owner_view{};
  add_active(&owner_view, follower.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  const auto configuration = canonical_configuration(owner, owner_view);
  const auto *follower_member = configuration.find(follower.node_id);
  require(follower_member != nullptr && !follower_member->leader_eligible,
          "standalone handoff fixture must leave only the owner eligible");

  ParticipantRegistry follower_view{};
  add_active(&follower_view, owner.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  SiteRoleElectionView view{};
  view.add(owner.node_id, configuration.roster_revision, true);
  view.add(follower.node_id, configuration.roster_revision, true);
  auto owner_heartbeat = heartbeat(
      owner.node_id, "boot-owner", 1, 1, 6000, 0, directory_revision,
      directory_digest);
  owner_heartbeat.election_view_digest =
      view.winner_contract_digest("site", owner.node_id);

  coordinator.set_local_directory_fence(directory_revision, directory_digest);
  require(coordinator.accept_heartbeat(follower_view, &configuration,
                                       owner_heartbeat, 1100) ==
              SiteHeartbeatAdmissionResult::OK,
          "canonical owner heartbeat was rejected during standalone handoff");
  coordinator.update(follower, follower_view, &configuration, true, 1100);
  require(coordinator.state().quorum_available &&
              !coordinator.state().local_leader &&
              coordinator.state().term == 1 &&
              std::strcmp(coordinator.state().leader_node_id,
                          owner.node_id) == 0,
          "standalone follower did not adopt the verified canonical winner at the equal initial term");
}

void standalone_joiners_use_the_bounded_handoff_through_seven_nodes() {
  constexpr uint32_t directory_revision = 6;
  constexpr uint32_t directory_digest = 0x61616161;
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto owner = identity("namo-node-owner");
    const auto joining = identity("namo-node-joining");
    ParticipantRegistry canonical{};
    add_active(&canonical, joining.node_id, ParticipantType::NAMO_NODE,
               ParticipantTrust::COMMUNITY_UNVERIFIED);

    std::array<std::array<char, 32>, 5> witness_ids{};
    std::array<std::array<char, 32>, 5> witness_boot_ids{};
    for (std::size_t index = 0; index + 2 < node_count; ++index) {
      std::snprintf(witness_ids[index].data(), witness_ids[index].size(),
                    "namo-node-witness-%u", static_cast<unsigned>(index));
      std::snprintf(witness_boot_ids[index].data(),
                    witness_boot_ids[index].size(), "boot-witness-%u",
                    static_cast<unsigned>(index));
      add_active(&canonical, witness_ids[index].data(),
                 ParticipantType::NAMO_NODE,
                 ParticipantTrust::COMMUNITY_UNVERIFIED);
    }
    auto configuration = canonical_configuration(owner, canonical);
    for (auto &member : configuration.members) {
      if (member.occupied &&
          std::strcmp(member.participant_id, owner.node_id) != 0)
        member.leader_eligible = false;
    }

    ParticipantRegistry joining_view{};
    add_active(&joining_view, owner.node_id, ParticipantType::NAMO_NODE,
               ParticipantTrust::COMMUNITY_UNVERIFIED);
    for (std::size_t index = 0; index + 2 < node_count; ++index)
      add_active(&joining_view, witness_ids[index].data(),
                 ParticipantType::NAMO_NODE,
                 ParticipantTrust::COMMUNITY_UNVERIFIED);

    SiteRoleElectionView view{};
    for (const auto &member : configuration.members) {
      if (member.occupied &&
          member.membership == ParticipantMembership::ACTIVE)
        view.add(member.participant_id, configuration.roster_revision, true);
    }
    const uint32_t contract_digest =
        view.winner_contract_digest("site", owner.node_id);

    SiteCoordinator coordinator{};
    ParticipantRegistry empty{};
    coordinator.update(joining, empty, 1000);
    coordinator.set_local_directory_fence(directory_revision,
                                          directory_digest);
    auto owner_heartbeat = heartbeat(
        owner.node_id, "boot-owner", 1, 1, 6000, 0, directory_revision,
        directory_digest);
    owner_heartbeat.election_view_digest = contract_digest;
    require(coordinator.accept_heartbeat(joining_view, &configuration,
                                         owner_heartbeat, 1100) ==
                SiteHeartbeatAdmissionResult::OK,
            "multi-node canonical owner heartbeat was rejected");
    for (std::size_t index = 0; index + 2 < node_count; ++index) {
      auto witness = heartbeat(
          witness_ids[index].data(), witness_boot_ids[index].data(), 1, 1,
          6000, 0, directory_revision, directory_digest);
      witness.election_view_digest = contract_digest;
      require(coordinator.accept_heartbeat(joining_view, &configuration,
                                           witness, 1100) ==
                  SiteHeartbeatAdmissionResult::OK,
              "multi-node canonical witness heartbeat was rejected");
    }
    coordinator.update(joining, joining_view, &configuration, true, 1100);
    require(coordinator.state().participants_expected == node_count &&
                coordinator.state().participants_online == node_count &&
                coordinator.state().quorum_available &&
                !coordinator.state().local_leader &&
                coordinator.state().term == 1 &&
                std::strcmp(coordinator.state().leader_node_id,
                            owner.node_id) == 0,
            "a standalone joiner did not converge without disturbing a 3-7 Node Site");
  }
}

void established_site_rejects_an_equal_term_replacement() {
  constexpr uint32_t directory_revision = 7;
  constexpr uint32_t directory_digest = 0x71717171;
  const auto local = identity("namo-node-local");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  add_active(&participants, "namo-node-b");
  auto configuration = canonical_configuration(local, participants);
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.leader_eligible =
        std::strcmp(member.participant_id, "namo-node-a") == 0;
  }

  SiteRoleElectionView initial_view{};
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE)
      initial_view.add(member.participant_id, configuration.roster_revision,
                       true);
  }
  const uint32_t initial_digest =
      initial_view.winner_contract_digest("site", "namo-node-a");
  auto a = heartbeat("namo-node-a", "boot-a", 1, 2, 6000, 0,
                     directory_revision, directory_digest);
  auto b = heartbeat("namo-node-b", "boot-b", 1, 2, 6000, 0,
                     directory_revision, directory_digest);
  a.election_view_digest = initial_digest;
  b.election_view_digest = initial_digest;

  SiteCoordinator coordinator{};
  coordinator.set_local_directory_fence(directory_revision, directory_digest);
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1000) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
          "established Site fixture heartbeats were rejected");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(coordinator.state().quorum_available &&
              coordinator.state().term == 2 &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-a") == 0,
          "established Site fixture did not elect its initial Leader");

  configuration.roster_revision++;
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.leader_eligible =
        std::strcmp(member.participant_id, "namo-node-b") == 0;
  }
  SiteRoleElectionView replacement_view{};
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE)
      replacement_view.add(member.participant_id,
                           configuration.roster_revision, true);
  }
  const uint32_t replacement_digest =
      replacement_view.winner_contract_digest("site", "namo-node-b");
  a.sequence = 2;
  b.sequence = 2;
  a.election_view_digest = replacement_digest;
  b.election_view_digest = replacement_digest;
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1100) ==
                  SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1100) ==
                  SiteHeartbeatAdmissionResult::OK,
          "replacement Site fixture heartbeats were rejected");
  coordinator.update(local, participants, &configuration, true, 1100);
  require(!coordinator.state().quorum_available &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().term == 2,
          "standalone handoff weakened the established Site term fence");
}

void strict_majority_is_bounded_for_two_through_seven_nodes() {
  for (uint8_t expected = 2; expected <= 7; ++expected) {
    const uint8_t required = strict_majority(expected);
    require(required == static_cast<uint8_t>(expected / 2U + 1U),
            "strict majority threshold is incorrect");
    require(!evaluate_role_quorum(expected,
                                  static_cast<uint8_t>(required - 1U))
                 .available,
            "a minority must not receive authority");
    const auto quorum = evaluate_role_quorum(expected, required);
    require(quorum.available && quorum.required == required &&
                quorum.expected == expected && quorum.online == required,
            "the smallest strict majority must receive authority");
  }
}

void eligible_nodes_elect_one_leader_and_expire_its_lease() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, 1100);
  require(coordinator.state().local_leader &&
              coordinator.state().participants_expected == 2 &&
              coordinator.state().participants_online == 1,
          "an offline remote Node must not become Site Leader");

  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-a", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "an active Node heartbeat should be admitted");
  coordinator.update(local, participants, 1000);
  require(!coordinator.state().local_leader &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().participants_online == 2 &&
              coordinator.state().term == 1,
          "a follower must wait for the selected remote Site winner term");
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-a", 2, 2), 1100) ==
              SiteHeartbeatAdmissionResult::OK,
          "the selected remote Site winner term should be admitted");
  coordinator.update(local, participants, 1100);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-a") == 0 &&
              coordinator.state().participants_online == 2 &&
              coordinator.state().term == 2,
          "the selected remote Site winner term should establish authority");

  coordinator.update(local, participants, 7100);
  require(coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-z") == 0 &&
              coordinator.state().participants_online == 1 &&
              coordinator.state().term == 3,
          "an expired remote lease must return authority to the local Node");
}

void offline_peers_keep_canonical_authority_closed_after_the_initial_probe() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-offline");
  const auto local = identity("namo-node-local");
  const auto configuration = canonical_configuration(local, participants);
  coordinator.set_local_directory_fence(3, 0x12345678);
  coordinator.update(local, participants, &configuration, 1000);
  require(coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().election_view_digest != 0 &&
              coordinator.state().election_endorsements == 1,
          "an unobserved peer must hold initial Site authority closed");
  coordinator.update(local, participants, &configuration, 6999);
  require(coordinator.state().leader_node_id[0] == '\0',
          "the initial authority probe ended before one lease");
  coordinator.update(local, participants, &configuration, 7000);
  require(!coordinator.state().local_leader &&
              coordinator.state().leader_node_id[0] == '\0' &&
              !coordinator.state().quorum_available &&
              coordinator.state().quorum_required == 2,
          "an isolated canonical survivor must not claim Site authority");
}

void admission_rejects_inactive_stale_and_replayed_heartbeats() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, 1000);

  require(coordinator.accept_heartbeat(
              participants, heartbeat("unknown", "boot-a", 1), 1000) ==
              SiteHeartbeatAdmissionResult::UNKNOWN_PARTICIPANT,
          "unknown participants must not affect Site authority");
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-a", 5, 3), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "a newer authority term should be admitted");
  coordinator.update(local, participants, 1000);
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-a", 5, 3), 1100) ==
              SiteHeartbeatAdmissionResult::DUPLICATE_OR_REORDERED,
          "a repeated sequence must be rejected");
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-a", 6, 2), 1100) ==
              SiteHeartbeatAdmissionResult::STALE_TERM,
          "an older authority term must be rejected");
  require(coordinator.accept_heartbeat(
              participants,
              heartbeat("namo-node-a", "boot-a", 6, 3, 6000, 6000),
              1100) == SiteHeartbeatAdmissionResult::EXPIRED,
          "an already expired lease must be rejected");
  require(participants.quarantine("namo-node-a") ==
              ParticipantRegistryResult::OK,
          "participant quarantine failed");
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-a", "boot-b", 1, 3), 1200) ==
              SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE,
          "quarantined Nodes must not participate in Site election");
}

void satellite_and_unapproved_community_nodes_never_lead() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  add_active(&participants, "satellite-a", ParticipantType::SATELLITE,
             ParticipantTrust::SATELLITE);
  add_active(&participants, "namo-node-b", ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  const auto local = identity("namo-node-z");
  coordinator.update(local, participants, 1000);
  require(coordinator.accept_heartbeat(
              participants, heartbeat("satellite-a", "boot-s", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(
                  participants, heartbeat("namo-node-b", "boot-b", 1),
                  1000) == SiteHeartbeatAdmissionResult::OK,
          "active non-leaders should still contribute Site health");
  const uint32_t authority_term = coordinator.state().term;
  require(coordinator.accept_heartbeat(
              participants, heartbeat("satellite-a", "boot-s", 2, 2),
              1100) ==
              SiteHeartbeatAdmissionResult::OK &&
              coordinator.state().term == authority_term,
          "a Satellite may contribute liveness but must not advance the Site term");
  coordinator.update(local, participants, 1100);
  require(coordinator.state().local_leader &&
              coordinator.state().participants_online == 3,
          "Satellite and unapproved community Nodes must not lead");

  require(participants.set_community_leader_eligible("namo-node-b", true) ==
              ParticipantRegistryResult::OK,
          "community leader opt-in failed");
  coordinator.update(local, participants, 1100);
  require(coordinator.state().leader_node_id[0] == '\0',
          "a newly eligible remote winner must issue its own Site term");
  require(coordinator.accept_heartbeat(
              participants, heartbeat("namo-node-b", "boot-b", 2, 2), 1200) ==
              SiteHeartbeatAdmissionResult::OK,
          "the approved community winner term was not admitted");
  coordinator.update(local, participants, 1200);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-b") == 0,
          "an explicitly approved community NAMO Node may lead");
}

void twenty_participants_fit_the_fixed_runtime_budget() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  char participant_id[32]{};
  char boot_id[32]{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    std::snprintf(participant_id, sizeof(participant_id), "namo-node-%02u",
                  static_cast<unsigned>(index));
    std::snprintf(boot_id, sizeof(boot_id), "boot-%02u",
                  static_cast<unsigned>(index));
    add_active(&participants, participant_id);
    require(coordinator.accept_heartbeat(
                participants, heartbeat(participant_id, boot_id, 1), 1000) ==
                SiteHeartbeatAdmissionResult::OK,
            "all bounded heartbeat slots should be available");
  }
  coordinator.update(identity("namo-node-local"), participants, 1000);
  require(coordinator.state().participants_expected == 20 &&
              coordinator.state().participants_online == 20,
          "the Site coordinator must account for twenty participants");
}

void site_leader_avoids_configuration_owner_when_possible() {
  const auto owner = identity("namo-node-a-owner");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-z-peer");
  const auto configuration = canonical_configuration(owner, participants);

  SiteCoordinator coordinator{};
  coordinator.update(owner, participants, &configuration, 1000);
  require(coordinator.accept_heartbeat(
              participants, &configuration,
              heartbeat("namo-node-z-peer", "boot-peer", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "non-owner Site candidate heartbeat should be admitted");
  coordinator.update(owner, participants, &configuration, 1000);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-z-peer") == 0,
          "a healthy non-owner should lead even with a larger Node ID");

  coordinator.update(owner, participants, &configuration, 7000);
  require(!coordinator.state().local_leader &&
              coordinator.state().leader_node_id[0] == '\0' &&
              !coordinator.state().quorum_available,
          "the configuration owner must not bypass canonical quorum");
}

void committed_roster_is_the_election_boundary() {
  SiteCoordinator coordinator{};
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  add_active(&participants, "namo-node-outside");
  const auto local = identity("namo-node-z");

  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(local, false, &base) ==
              SiteConfigurationResult::OK,
          "Site configuration bootstrap failed");
  ParticipantRegistry canonical{};
  add_active(&canonical, "namo-node-a");
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(local, canonical, base, &configuration) ==
              SiteConfigurationResult::OK,
          "canonical roster commit failed");

  require(coordinator.accept_heartbeat(
              participants, &configuration,
              heartbeat("namo-node-outside", "boot-outside", 1), 1000) ==
              SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE,
          "a locally active peer outside the committed roster must be rejected");
  require(coordinator.accept_heartbeat(
              participants, &configuration,
              heartbeat("namo-node-a", "boot-a", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "a committed active roster member must be admitted");
  coordinator.update(local, participants, &configuration, 1000);
  require(coordinator.state().configuration_ready &&
              coordinator.state().roster_revision == 2 &&
              coordinator.state().participants_expected == 2 &&
              coordinator.state().participants_online == 2 &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-a") == 0,
          "election and health must use the canonical committed roster");
}

void canonical_roster_overrides_pair_local_leader_flags() {
  const auto owner = identity("namo-node-owner");
  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "canonical leader fixture bootstrap failed");
  ParticipantRegistry canonical{};
  add_active(&canonical, "namo-node-follower", ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, canonical, base, &configuration) ==
              SiteConfigurationResult::OK,
          "canonical leader fixture commit failed");

  ParticipantRegistry local_view{};
  add_active(&local_view, "namo-node-owner", ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  SiteCoordinator owner_coordinator{};
  SiteCoordinator follower_coordinator{};
  owner_coordinator.update(owner, canonical, &configuration, 1000);
  follower_coordinator.update(identity("namo-node-follower"), local_view,
                              &configuration, 1000);
  require(follower_coordinator.state().leader_node_id[0] == '\0' &&
              !follower_coordinator.state().local_leader,
          "an ineligible follower must not elect itself while owner is offline");
  require(follower_coordinator.accept_heartbeat(
              local_view, &configuration,
              heartbeat("namo-node-owner", "boot-owner", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "canonical eligibility must admit the owner despite pair-local trust");
  follower_coordinator.update(identity("namo-node-follower"), local_view,
                              &configuration, 1000);
  require(owner_coordinator.accept_heartbeat(
              canonical, &configuration,
              heartbeat("namo-node-follower", "boot-follower", 1,
                        follower_coordinator.state().term),
              1000) == SiteHeartbeatAdmissionResult::OK,
          "the ineligible follower must still contribute Site health");
  owner_coordinator.update(owner, canonical, &configuration, 1000);
  require(follower_coordinator.accept_heartbeat(
              local_view, &configuration,
              heartbeat(owner.node_id, "boot-owner", 2,
                        owner_coordinator.state().term),
              1100) == SiteHeartbeatAdmissionResult::OK,
          "the elected Site winner term was not propagated to its follower");
  follower_coordinator.update(identity("namo-node-follower"), local_view,
                              &configuration, 1100);
  require(!follower_coordinator.state().local_leader &&
              std::strcmp(follower_coordinator.state().leader_node_id,
                          "namo-node-owner") == 0 &&
              std::strcmp(owner_coordinator.state().leader_node_id,
                          "namo-node-owner") == 0 &&
              owner_coordinator.state().term ==
                  follower_coordinator.state().term &&
              owner_coordinator.state().participants_online == 2 &&
              follower_coordinator.state().participants_online == 2,
          "both Site views must converge on the canonical Leader and term");
}

void indirect_canonical_members_converge_on_one_site_leader() {
  const auto owner = identity("namo-node-owner");
  const auto local = identity("namo-node-z-local");
  const auto remote = identity("namo-node-a-remote");

  ParticipantRegistry owner_view{};
  add_active(&owner_view, local.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  add_active(&owner_view, remote.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  require(owner_view.set_community_leader_eligible(local.node_id, true) ==
                  ParticipantRegistryResult::OK &&
              owner_view.set_community_leader_eligible(remote.node_id, true) ==
                  ParticipantRegistryResult::OK,
          "indirect Site candidates must be explicitly leader eligible");
  const auto configuration = canonical_configuration(owner, owner_view);

  ParticipantRegistry local_pair_view{};
  add_active(&local_pair_view, owner.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  require(local_pair_view.find(remote.node_id) == nullptr,
          "the indirect peer must not be inserted into the direct pairing registry");

  SiteCoordinator coordinator{};
  coordinator.update(local, local_pair_view, &configuration, 1000);
  require(coordinator.accept_heartbeat(
              local_pair_view, &configuration,
              heartbeat(remote.node_id, "boot-remote", 1), 1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "an authenticated indirect canonical member must enter Site election");
  coordinator.update(local, local_pair_view, &configuration, 1000);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          remote.node_id) == 0 &&
              coordinator.state().participants_online == 2,
          "indirect canonical peers must converge on the deterministic Site Leader");
}

void canonical_leader_recovers_an_expired_higher_term() {
  const auto owner = identity("namo-node-owner");
  const auto follower = identity("namo-node-follower");
  SiteConfigurationSnapshot base{};
  require(initialize_site_configuration(owner, false, &base) ==
              SiteConfigurationResult::OK,
          "term recovery fixture bootstrap failed");
  ParticipantRegistry canonical_participants{};
  add_active(&canonical_participants, follower.node_id,
             ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  SiteConfigurationSnapshot configuration{};
  require(commit_local_site_roster(owner, canonical_participants, base,
                                   &configuration) ==
              SiteConfigurationResult::OK,
          "term recovery canonical roster failed");

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
  require(follower_found, "term recovery follower fixture is missing");

  ParticipantRegistry follower_view{};
  add_active(&follower_view, owner.node_id, ParticipantType::NAMO_NODE,
             ParticipantTrust::COMMUNITY_UNVERIFIED);
  SiteCoordinator coordinator{};
  coordinator.update(follower, follower_view, &previous_configuration, 1000);
  require(coordinator.state().leader_node_id[0] == '\0',
          "legacy follower fixture must first probe for current authority");
  coordinator.update(follower, follower_view, &previous_configuration, 7000);
  require(!coordinator.state().local_leader &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().term == 1 &&
              !coordinator.state().quorum_available,
          "an isolated Site follower must wait for canonical quorum");
  require(coordinator.accept_heartbeat(
              follower_view, &previous_configuration,
              heartbeat(owner.node_id, "boot-owner-old", 1, 3), 7000) ==
              SiteHeartbeatAdmissionResult::OK,
          "legacy higher term fixture must be admitted");
  coordinator.update(follower, follower_view, &previous_configuration, 7000);
  require(coordinator.state().local_leader &&
              coordinator.state().term == 4,
          "legacy follower fixture must fence the observed higher term");

  coordinator.update(follower, follower_view, &configuration, 7001);
  require(!coordinator.state().local_leader &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().term == 4,
          "a replacement Site winner must issue its own higher term");
  require(coordinator.accept_heartbeat(
              follower_view, &configuration,
              heartbeat(owner.node_id, "boot-owner-old", 2, 5), 7002) ==
              SiteHeartbeatAdmissionResult::OK,
          "replacement Site winner term must be admitted");
  coordinator.update(follower, follower_view, &configuration, 7002);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          owner.node_id) == 0 &&
              coordinator.state().term == 5,
          "canonical Site winner did not establish its issued term");
  coordinator.update(follower, follower_view, &configuration, 13002);
  require(coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().term == 5,
          "expired authority must leave the poisoned term without a Leader");

  require(coordinator.accept_heartbeat(
              follower_view, &configuration,
              heartbeat(owner.node_id, "boot-owner-new", 1, 6), 13003) ==
              SiteHeartbeatAdmissionResult::OK,
          "an eligible restarted owner must recover an expired higher term");
  coordinator.update(follower, follower_view, &configuration, 13003);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          owner.node_id) == 0 &&
              coordinator.state().term == 6 &&
              coordinator.state().participants_online == 2,
          "the follower must converge on a monotonic term after the canonical owner restarts");
}

void mismatched_election_views_fail_closed_until_quorum_endorses_one_view() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  add_active(&participants, "namo-node-b");
  const auto configuration = canonical_configuration(local, participants);

  RoleElectionView view{};
  view.add(local.node_id, configuration.roster_revision, true);
  view.add("namo-node-a", configuration.roster_revision, true);
  view.add("namo-node-b", configuration.roster_revision, true);
  const uint32_t shared_digest =
      view.winner_contract_digest("site", "namo-node-a");

  auto a = heartbeat("namo-node-a", "boot-a", 1);
  a.election_view_digest = shared_digest ^ 1U;
  auto b = heartbeat("namo-node-b", "boot-b", 1);
  b.election_view_digest = shared_digest ^ 2U;

  SiteCoordinator coordinator{};
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1000) ==
              SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
          "mismatched election views were not admitted for fencing");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(!coordinator.state().quorum_available &&
              coordinator.state().leader_node_id[0] == '\0' &&
              coordinator.state().election_fenced,
          "a raw heartbeat majority bypassed the election-view fence");

  a.sequence = 2;
  b.sequence = 2;
  a.election_view_digest = shared_digest;
  b.election_view_digest = shared_digest;
  require(coordinator.accept_heartbeat(participants, &configuration, a, 1100) ==
              SiteHeartbeatAdmissionResult::OK &&
              coordinator.accept_heartbeat(participants, &configuration, b,
                                           1100) ==
                  SiteHeartbeatAdmissionResult::OK,
          "matching election views were not admitted");
  coordinator.update(local, participants, &configuration, true, 1100);
  require(coordinator.state().quorum_available &&
              coordinator.state().election_endorsements == 3 &&
              std::strcmp(coordinator.state().leader_node_id,
                          "namo-node-a") == 0,
          "an endorsed election view did not restore one deterministic leader");
}

void returning_member_preserves_an_established_site_leader_during_view_rollout() {
  constexpr uint32_t directory_revision = 7;
  constexpr uint32_t directory_digest = 0x71717171;
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    std::array<std::array<char, 32>, 6> node_ids{};
    std::array<std::array<char, 32>, 6> boot_ids{};
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      std::snprintf(node_ids[index].data(), node_ids[index].size(),
                    "namo-node-%02u", static_cast<unsigned>(index));
      std::snprintf(boot_ids[index].data(), boot_ids[index].size(),
                    "boot-%02u", static_cast<unsigned>(index));
      add_active(&participants, node_ids[index].data());
    }
    const auto configuration = canonical_configuration(local, participants);

    SiteRoleElectionView canonical_view{};
    canonical_view.add(local.node_id, configuration.roster_revision, true);
    for (std::size_t index = 0; index + 1 < node_count; ++index)
      canonical_view.add(node_ids[index].data(),
                         configuration.roster_revision, true);
    const uint32_t canonical_digest =
        canonical_view.winner_contract_digest("site", node_ids[0].data());

    SiteCoordinator coordinator{};
    coordinator.set_local_directory_fence(directory_revision,
                                          directory_digest);
    const std::size_t remote_count = node_count - 1;
    const std::size_t survivor_remote_count =
        strict_majority(static_cast<uint8_t>(node_count)) - 1U;
    for (std::size_t index = 0; index < survivor_remote_count; ++index) {
      auto peer = heartbeat(node_ids[index].data(), boot_ids[index].data(), 1,
                            1, 6000, 0, directory_revision,
                            directory_digest);
      peer.election_view_digest = canonical_digest;
      require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
              "canonical Site heartbeat was rejected before member return");
    }
    coordinator.update(local, participants, &configuration, true, 1000);
    require(coordinator.state().quorum_available &&
                std::strcmp(coordinator.state().leader_node_id,
                            node_ids[0].data()) == 0,
            "surviving Site quorum did not establish its canonical leader");
    const uint32_t stable_term = coordinator.state().term;

    uint32_t now_ms = 1100;
    for (std::size_t index = survivor_remote_count; index < remote_count;
         ++index, now_ms += 100) {
      auto returning = heartbeat(node_ids[index].data(),
                                 boot_ids[index].data(), 1, stable_term, 6000,
                                 0, directory_revision, directory_digest);
      returning.election_view_digest =
          canonical_digest ^ static_cast<uint32_t>(0x55AA5500U + index);
      require(coordinator.accept_heartbeat(participants, &configuration,
                                           returning, now_ms) ==
                  SiteHeartbeatAdmissionResult::OK,
              "returning Site member heartbeat was rejected");
      coordinator.update(local, participants, &configuration, true, now_ms);
      require(coordinator.state().participants_online == index + 2 &&
                  coordinator.state().quorum_available &&
                  coordinator.state().term == stable_term &&
                  std::strcmp(coordinator.state().leader_node_id,
                              node_ids[0].data()) == 0,
              "stale returning members suspended the established Site Leader");
    }

    for (std::size_t index = survivor_remote_count; index < remote_count;
         ++index, now_ms += 100) {
      auto converged = heartbeat(node_ids[index].data(),
                                 boot_ids[index].data(), 2, stable_term, 6000,
                                 0, directory_revision, directory_digest);
      converged.election_view_digest = canonical_digest;
      require(coordinator.accept_heartbeat(participants, &configuration,
                                           converged, now_ms) ==
                  SiteHeartbeatAdmissionResult::OK,
              "returning Site member did not adopt the canonical contract");
      coordinator.update(local, participants, &configuration, true, now_ms);
      require(coordinator.state().quorum_available &&
                  coordinator.state().term == stable_term &&
                  std::strcmp(coordinator.state().leader_node_id,
                              node_ids[0].data()) == 0,
              "Site Leader changed while returning members converged");
    }
  }
}

void mixed_v11_v12_site_election_contracts_converge_through_seven_nodes() {
  constexpr uint32_t directory_revision = 12;
  constexpr uint32_t directory_digest = 0x12121212;
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    std::array<std::array<char, 32>, 6> node_ids{};
    std::array<std::array<char, 32>, 6> boot_ids{};
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      std::snprintf(node_ids[index].data(), node_ids[index].size(),
                    "namo-node-%02u", static_cast<unsigned>(index));
      std::snprintf(boot_ids[index].data(), boot_ids[index].size(),
                    "boot-%02u", static_cast<unsigned>(index));
      add_active(&participants, node_ids[index].data());
    }
    const auto configuration = canonical_configuration(local, participants);
    SiteRoleElectionView current_view{};
    SiteRoleElectionView v11_view{};
    const uint64_t directory_qualifier =
        (static_cast<uint64_t>(directory_revision) << 32U) | directory_digest;
    current_view.add(local.node_id, configuration.roster_revision, true);
    v11_view.add(local.node_id, directory_qualifier, true);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      current_view.add(node_ids[index].data(), configuration.roster_revision,
                       true);
      v11_view.add(node_ids[index].data(), directory_qualifier, true);
    }
    const uint32_t current_digest = current_view.winner_contract_digest(
        "site", node_ids[0].data());
    const uint32_t v11_digest =
        v11_view.winner_contract_digest("site", node_ids[0].data());

    SiteCoordinator coordinator{};
    coordinator.set_local_directory_fence(directory_revision,
                                          directory_digest);
    for (std::size_t index = 0; index + 1 < node_count; ++index) {
      auto peer = heartbeat(node_ids[index].data(), boot_ids[index].data(), 1,
                            1, 6000, 0, directory_revision,
                            directory_digest);
      peer.election_contract_version = index % 2 == 0 ? 2 : 3;
      peer.election_view_digest = peer.election_contract_version == 2
                                      ? v11_digest
                                      : current_digest;
      require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
              "mixed-version Site heartbeat was rejected");
    }
    coordinator.update(local, participants, &configuration, true, 1000);
    require(coordinator.state().participants_online == node_count &&
                coordinator.state().quorum_available &&
                coordinator.state().election_endorsements == node_count &&
                std::strcmp(coordinator.state().leader_node_id,
                            node_ids[0].data()) == 0,
            "mixed v11/v12 Site contracts failed to converge at 3-7 Nodes");
  }
}

void returning_preferred_candidate_does_not_preempt_an_active_site_leader() {
  constexpr uint32_t directory_revision = 8;
  constexpr uint32_t directory_digest = 0x81818181;
  for (std::size_t node_count = 3; node_count <= 7; ++node_count) {
    const auto local = identity("namo-node-z");
    ParticipantRegistry participants{};
    std::array<std::array<char, 32>, 6> survivor_ids{};
    std::array<std::array<char, 32>, 6> survivor_boot_ids{};
    const std::size_t survivor_remote_count =
        strict_majority(static_cast<uint8_t>(node_count)) - 1U;
    for (std::size_t index = 0; index < survivor_remote_count; ++index) {
      std::snprintf(survivor_ids[index].data(),
                    survivor_ids[index].size(), "namo-node-%02u",
                    static_cast<unsigned>(index + 1));
      std::snprintf(survivor_boot_ids[index].data(),
                    survivor_boot_ids[index].size(), "boot-%02u",
                    static_cast<unsigned>(index + 1));
      add_active(&participants, survivor_ids[index].data());
    }
    add_active(&participants, "namo-node-00");
    for (std::size_t index = survivor_remote_count + 1;
         index + 1 < node_count; ++index) {
      char node_id[32]{};
      std::snprintf(node_id, sizeof(node_id), "namo-node-%02u",
                    static_cast<unsigned>(index + 1));
      add_active(&participants, node_id);
    }
    const auto configuration = canonical_configuration(local, participants);

    SiteRoleElectionView established_view{};
    for (const auto &member : configuration.members) {
      if (member.occupied &&
          member.membership == ParticipantMembership::ACTIVE)
        established_view.add(member.participant_id,
                             configuration.roster_revision, true);
    }
    const char *incumbent = survivor_ids[0].data();
    const uint32_t established_digest =
        established_view.winner_contract_digest("site", incumbent);
    const uint32_t returning_digest =
        established_view.winner_contract_digest("site", "namo-node-00");

    SiteCoordinator coordinator{};
    coordinator.set_local_directory_fence(directory_revision,
                                          directory_digest);
    for (std::size_t index = 0; index < survivor_remote_count; ++index) {
      auto peer = heartbeat(survivor_ids[index].data(),
                            survivor_boot_ids[index].data(), 1, 1, 6000, 0,
                            directory_revision, directory_digest);
      peer.election_view_digest = established_digest;
      require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                           1000) ==
                  SiteHeartbeatAdmissionResult::OK,
              "incumbent Site quorum heartbeat was rejected");
    }
    coordinator.update(local, participants, &configuration, true, 1000);
    require(coordinator.state().quorum_available &&
                std::strcmp(coordinator.state().leader_node_id, incumbent) ==
                    0,
            "preferred-return fixture did not establish its incumbent");
    const uint32_t stable_term = coordinator.state().term;

    auto returning = heartbeat("namo-node-00", "boot-00-new", 1,
                               stable_term, 6000, 0, directory_revision,
                               directory_digest);
    returning.election_view_digest = returning_digest;
    require(coordinator.accept_heartbeat(participants, &configuration,
                                         returning, 1100) ==
                SiteHeartbeatAdmissionResult::OK,
            "preferred returning Site candidate was rejected");
    coordinator.update(local, participants, &configuration, true, 1100);
    require(coordinator.state().quorum_available &&
                coordinator.state().term == stable_term &&
                std::strcmp(coordinator.state().leader_node_id, incumbent) ==
                    0,
            "a preferred returning candidate preempted the active Site Leader");
  }
}

void credential_incomplete_candidates_never_receive_site_authority() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  const auto configuration = canonical_configuration(local, participants);
  auto peer = heartbeat("namo-node-a", "boot-a", 1);
  peer.credentials_complete = false;

  SiteCoordinator coordinator{};
  require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                       1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "credential-incomplete heartbeat should remain an online witness");
  coordinator.update(local, participants, &configuration, true, 1000);
  require(coordinator.state().leader_node_id[0] == '\0',
          "the initial authority probe must not treat an incomplete peer as a candidate");
  peer.sequence = 2;
  require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                       7000) ==
              SiteHeartbeatAdmissionResult::OK,
          "credential-incomplete witness lease could not be refreshed");
  coordinator.update(local, participants, &configuration, true, 7000);
  require(coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          local.node_id) == 0,
          "a credential-incomplete remote became Site Leader");
}

void credential_incomplete_local_candidate_yields_site_authority() {
  const auto local = identity("namo-node-z");
  ParticipantRegistry participants{};
  add_active(&participants, "namo-node-a");
  const auto configuration = canonical_configuration(local, participants);

  RoleElectionView view{};
  view.add(local.node_id, configuration.roster_revision, true);
  view.add("namo-node-a", configuration.roster_revision, true);
  auto peer = heartbeat("namo-node-a", "boot-a", 1);
  peer.election_view_digest =
      view.winner_contract_digest("site", "namo-node-a");

  SiteCoordinator coordinator{};
  require(coordinator.accept_heartbeat(participants, &configuration, peer,
                                       1000) ==
              SiteHeartbeatAdmissionResult::OK,
          "credential-complete Site peer was not admitted");
  coordinator.update(local, participants, &configuration, false, 1000);
  require(!coordinator.state().local_leader &&
              std::strcmp(coordinator.state().leader_node_id,
                          peer.participant_id) == 0 &&
              coordinator.state().quorum_available,
          "a credential-incomplete local Node retained Site authority");
}

}  // namespace

void run_site_coordinator_tests() {
  standalone_node_uses_the_site_authority_contract();
  standalone_follower_adopts_the_canonical_site_winner_at_equal_term();
  standalone_joiners_use_the_bounded_handoff_through_seven_nodes();
  established_site_rejects_an_equal_term_replacement();
  strict_majority_is_bounded_for_two_through_seven_nodes();
  eligible_nodes_elect_one_leader_and_expire_its_lease();
  offline_peers_keep_canonical_authority_closed_after_the_initial_probe();
  admission_rejects_inactive_stale_and_replayed_heartbeats();
  satellite_and_unapproved_community_nodes_never_lead();
  twenty_participants_fit_the_fixed_runtime_budget();
  site_leader_avoids_configuration_owner_when_possible();
  committed_roster_is_the_election_boundary();
  canonical_roster_overrides_pair_local_leader_flags();
  indirect_canonical_members_converge_on_one_site_leader();
  canonical_leader_recovers_an_expired_higher_term();
  minority_higher_directory_fork_cannot_take_site_leadership();
  minority_higher_directory_forks_are_rejected_through_seven_nodes();
  current_site_leader_can_publish_a_new_directory_before_replica_quorum();
  zero_election_digest_is_not_a_fence_bypass();
  mismatched_election_views_fail_closed_until_quorum_endorses_one_view();
  returning_member_preserves_an_established_site_leader_during_view_rollout();
  mixed_v11_v12_site_election_contracts_converge_through_seven_nodes();
  returning_preferred_candidate_does_not_preempt_an_active_site_leader();
  credential_incomplete_candidates_never_receive_site_authority();
  credential_incomplete_local_candidate_yields_site_authority();
}
