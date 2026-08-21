#include "site_authority.h"
#include "site_heartbeat.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

constexpr uint32_t NOW_MS = 1000;
constexpr const char *LOCAL_ID = "namo-node-local";
constexpr const char *REMOTE_ID = "namo-node-remote";

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_authority_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity local_identity() {
  PlatformIdentity value{};
  std::strcpy(value.site_id, "site-home");
  std::strcpy(value.node_id, LOCAL_ID);
  return value;
}

SiteConfigurationSnapshot configuration_with_remote() {
  SiteConfigurationSnapshot value{};
  require(initialize_site_configuration(local_identity(), false, &value) ==
              SiteConfigurationResult::OK,
          "configuration bootstrap failed");
  for (auto &member : value.members) {
    if (member.occupied)
      continue;
    member.occupied = true;
    std::strcpy(member.participant_id, REMOTE_ID);
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    return value;
  }
  require(false, "remote member slot missing");
  return value;
}

ParticipantEndpointCache endpoint_cache(
    const char *participant_id, uint32_t authority_epoch, uint32_t generation,
    uint64_t digest) {
  ParticipantEndpointCache endpoints{};
  require(endpoints.observe_authenticated(participant_id, 0xC0A80102U, 49191,
                                           NOW_MS),
          "authenticated endpoint setup failed");
  require(endpoints.observe_floorplan_replica(
              participant_id, authority_epoch, generation, digest, 0, 0, 0,
              0, false),
          "endpoint authority setup failed");
  require(endpoints.observe_site_credentials(
              participant_id, SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION,
              true),
          "endpoint credential readiness setup failed");
  return endpoints;
}

void add_active_member(SiteConfigurationSnapshot *configuration,
                       const char *participant_id) {
  require(configuration != nullptr && participant_id != nullptr,
          "active member fixture is invalid");
  for (auto &member : configuration->members) {
    if (member.occupied)
      continue;
    member.occupied = true;
    std::strcpy(member.participant_id, participant_id);
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    return;
  }
  require(false, "active member fixture exceeded capacity");
}

SiteConfigurationSnapshot exact_owner_handover(
    const SiteConfigurationSnapshot &current, const char *next_owner) {
  SiteConfigurationSnapshot updated = current;
  require(next_owner != nullptr && current.find(next_owner) != nullptr &&
              current.authority_epoch != UINT32_MAX &&
              current.generation != UINT32_MAX,
          "owner handover fixture is invalid");
  std::strcpy(updated.configuration_owner_node_id, next_owner);
  ++updated.authority_epoch;
  ++updated.generation;
  return updated;
}

void observe_authority_endpoint(
    ParticipantEndpointCache *endpoints, const char *participant_id,
    const SiteConfigurationSnapshot &configuration, uint32_t ipv4) {
  require(endpoints != nullptr, "authority endpoint cache is missing");
  endpoints->observe_authenticated(participant_id, ipv4, 49191, NOW_MS);
  require(ParticipantEndpointCache::is_online(
              endpoints->find(participant_id), NOW_MS) &&
              endpoints->observe_floorplan_replica(
                  participant_id, configuration.authority_epoch,
                  configuration.generation,
                  site_configuration_digest(configuration), 0, 0, 0, 0,
                  false) &&
              endpoints->observe_site_topology(
                  participant_id, configuration.roster_revision,
                  site_topology_digest(configuration)) &&
              endpoints->observe_site_credentials(
                  participant_id,
                  SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION, true),
          "authority endpoint fixture setup failed");
}

SiteAuthorityReplicationCandidate candidate(uint32_t authority_epoch,
                                            uint32_t generation,
                                            uint32_t delay_ms = 2500) {
  SiteAuthorityReplicationCandidate value{};
  value.available = true;
  std::strcpy(value.source_node_id, REMOTE_ID);
  value.authority_epoch = authority_epoch;
  value.generation = generation;
  value.configuration_digest =
      (static_cast<uint64_t>(authority_epoch) << 32U) | generation;
  value.slot_delay_ms = delay_ms;
  return value;
}

void unavailable_inputs_fail_closed() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  const ParticipantEndpointCache endpoints{};
  require(evaluate_site_authority(nullptr, &configuration, &endpoints,
                                  NOW_MS)
                  .state == SiteAuthorityState::NOT_READY,
          "missing identity must fail closed");
  require(evaluate_site_authority(&identity, nullptr, &endpoints, NOW_MS)
                  .state == SiteAuthorityState::NOT_READY,
          "missing configuration must fail closed");
  require(evaluate_site_authority(&identity, &configuration, nullptr, NOW_MS)
                  .state == SiteAuthorityState::NOT_READY,
          "missing endpoint cache must fail closed");
}

void standalone_and_offline_members_are_consistent() {
  const auto identity = local_identity();
  SiteConfigurationSnapshot standalone{};
  require(initialize_site_configuration(identity, false, &standalone) ==
              SiteConfigurationResult::OK,
          "standalone bootstrap failed");
  const ParticipantEndpointCache endpoints{};
  const auto standalone_authority =
      evaluate_site_authority(&identity, &standalone, &endpoints, NOW_MS);
  require(standalone_authority.state == SiteAuthorityState::STANDALONE &&
              standalone_authority.consistent(),
          "standalone authority must be consistent");

  const auto configuration = configuration_with_remote();
  const auto offline_authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(offline_authority.state == SiteAuthorityState::CONSISTENT,
          "offline members must not create an authority conflict");
}

void matching_online_member_is_consistent() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  const auto endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch, configuration.generation,
      site_configuration_digest(configuration));
  const auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONSISTENT &&
              authority.consistent(),
          "matching online member must be consistent");
}

void revision_lag_is_convergence_not_conflict() {
  const auto identity = local_identity();
  auto configuration = configuration_with_remote();
  configuration.generation = 2;
  auto endpoints = endpoint_cache(REMOTE_ID, configuration.authority_epoch, 1,
                                  0x1234U);
  auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.reason == SiteAuthorityReason::PEER_BEHIND,
          "older peer must be classified as converging");

  endpoints = endpoint_cache(REMOTE_ID, configuration.authority_epoch, 3,
                             0x5678U);
  authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.reason == SiteAuthorityReason::LOCAL_BEHIND,
          "newer peer must be classified as converging");
}

void unfenced_peer_is_converging() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  const auto endpoints =
      endpoint_cache(REMOTE_ID, 0, configuration.generation, 0);
  const auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.reason == SiteAuthorityReason::UNFENCED_PEER,
          "legacy peer must remain non-authoritative while converging");
}

void equal_revision_digest_divergence_is_conflict() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  const auto digest = site_configuration_digest(configuration);
  const auto endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch, configuration.generation,
      digest ^ 0x1U);
  const auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONFLICT &&
              authority.reason == SiteAuthorityReason::DIVERGENT_DIGEST &&
              !authority.consistent(),
          "equal revision digest divergence must be a conflict");
}

void activation_gate_distinguishes_convergence_from_conflict() {
  const auto configuration = configuration_with_remote();
  const auto digest = site_configuration_digest(configuration);

  auto matching_endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch, configuration.generation,
      digest);
  const auto *matching = matching_endpoints.find(REMOTE_ID);
  SiteAuthoritySnapshot authority{};
  authority.evaluated = true;
  authority.state = SiteAuthorityState::CONSISTENT;
  require(evaluate_site_activation_authority_gate(
              authority, configuration, matching, true) ==
              SiteActivationAuthorityGateResult::OK,
          "matching activation authority must pass");

  authority.state = SiteAuthorityState::CONVERGING;
  require(evaluate_site_activation_authority_gate(
              authority, configuration, matching, true) ==
              SiteActivationAuthorityGateResult::CONTINUITY_UNAVAILABLE,
          "converging Site authority must remain retryable");

  authority.state = SiteAuthorityState::CONFLICT;
  require(evaluate_site_activation_authority_gate(
              authority, configuration, matching, true) ==
              SiteActivationAuthorityGateResult::CONFLICT,
          "conflicting Site authority must remain a hard conflict");

  authority.state = SiteAuthorityState::CONSISTENT;
  auto behind_endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch,
      configuration.generation - 1, digest);
  require(evaluate_site_activation_authority_gate(
              authority, configuration, behind_endpoints.find(REMOTE_ID),
              true) ==
              SiteActivationAuthorityGateResult::CONTINUITY_UNAVAILABLE,
          "an older participant endpoint must be treated as convergence");

  auto divergent_endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch, configuration.generation,
      digest ^ 0x1U);
  require(evaluate_site_activation_authority_gate(
              authority, configuration, divergent_endpoints.find(REMOTE_ID),
              true) == SiteActivationAuthorityGateResult::CONFLICT,
          "equal-revision endpoint divergence must remain a conflict");
}

void third_through_seventh_member_wait_for_existing_peers() {
  constexpr const char *CANDIDATE_ID = "namo-node-candidate";
  for (uint8_t total_members = 3; total_members <= 7; ++total_members) {
    const auto identity = local_identity();
    SiteConfigurationSnapshot configuration{};
    require(initialize_site_configuration(identity, false, &configuration) ==
                SiteConfigurationResult::OK,
            "multi-member activation bootstrap failed");
    configuration.generation = 20U + total_members;

    auto add_member = [&configuration](const char *participant_id,
                                       ParticipantMembership membership) {
      for (auto &member : configuration.members) {
        if (member.occupied)
          continue;
        member.occupied = true;
        std::strncpy(member.participant_id, participant_id,
                     sizeof(member.participant_id) - 1);
        member.type = ParticipantType::NAMO_NODE;
        member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
        member.membership = membership;
        member.leader_eligible = true;
        return true;
      }
      return false;
    };
    require(add_member(CANDIDATE_ID, ParticipantMembership::QUARANTINED),
            "activation candidate slot missing");

    char lagging_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    const uint8_t existing_remote_count = total_members - 2;
    for (uint8_t index = 0; index < existing_remote_count; ++index) {
      char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      std::snprintf(participant_id, sizeof(participant_id),
                    "namo-node-existing-%u", static_cast<unsigned>(index));
      require(add_member(participant_id, ParticipantMembership::ACTIVE),
              "existing active member slot missing");
    }

    ParticipantEndpointCache endpoints{};
    const auto digest = site_configuration_digest(configuration);
    require(endpoints.observe_authenticated(CANDIDATE_ID, 0xC0A80201U,
                                             49191, NOW_MS) &&
                endpoints.observe_floorplan_replica(
                    CANDIDATE_ID, configuration.authority_epoch,
                    configuration.generation, digest, 0, 0, 0, 0, false),
            "activation candidate endpoint setup failed");
    for (uint8_t index = 0; index < existing_remote_count; ++index) {
      char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
      std::snprintf(participant_id, sizeof(participant_id),
                    "namo-node-existing-%u", static_cast<unsigned>(index));
      require(endpoints.observe_authenticated(
                  participant_id, 0xC0A80300U + index, 49191, NOW_MS),
              "existing active endpoint setup failed");
      const bool lagging = index + 1 == existing_remote_count;
      require(endpoints.observe_floorplan_replica(
                  participant_id, configuration.authority_epoch,
                  lagging ? configuration.generation - 1
                          : configuration.generation,
                  digest, 0, 0, 0, 0, false),
              "existing active authority setup failed");
      if (lagging)
        std::strncpy(lagging_id, participant_id, sizeof(lagging_id) - 1);
    }

    auto authority = evaluate_site_authority(
        &identity, &configuration, &endpoints, NOW_MS);
    require(authority.state == SiteAuthorityState::CONVERGING &&
                evaluate_site_activation_authority_gate(
                    authority, configuration, endpoints.find(CANDIDATE_ID),
                    true) == SiteActivationAuthorityGateResult::
                                 CONTINUITY_UNAVAILABLE,
            "an existing peer lag must defer sequential activation");

    require(endpoints.observe_floorplan_replica(
                lagging_id, configuration.authority_epoch,
                configuration.generation, digest, 0, 0, 0, 0, false),
            "lagging peer convergence setup failed");
    authority = evaluate_site_authority(
        &identity, &configuration, &endpoints, NOW_MS);
    require(authority.consistent() &&
                evaluate_site_activation_authority_gate(
                    authority, configuration, endpoints.find(CANDIDATE_ID),
                    true) == SiteActivationAuthorityGateResult::OK,
            "sequential activation must open after all existing peers converge");
  }
}

void sequential_activation_fault_position_matrix() {
  constexpr const char *CANDIDATE_ID = "namo-node-candidate";
  enum class PeerFault : uint8_t {
    BEHIND = 0,
    AHEAD,
    UNFENCED,
    DIVERGENT,
    OFFLINE,
  };
  const std::array<PeerFault, 5> faults{
      PeerFault::BEHIND,
      PeerFault::AHEAD,
      PeerFault::UNFENCED,
      PeerFault::DIVERGENT,
      PeerFault::OFFLINE,
  };

  for (uint8_t total_members = 3; total_members <= 7; ++total_members) {
    const uint8_t existing_remote_count = total_members - 2;
    for (uint8_t fault_index = 0; fault_index < existing_remote_count;
         ++fault_index) {
      for (const auto fault : faults) {
        const auto identity = local_identity();
        SiteConfigurationSnapshot configuration{};
        require(initialize_site_configuration(identity, false,
                                              &configuration) ==
                    SiteConfigurationResult::OK,
                "activation fault matrix bootstrap failed");
        configuration.generation = 100U + total_members;

        auto add_member = [&configuration](
                              const char *participant_id,
                              ParticipantMembership membership) {
          for (auto &member : configuration.members) {
            if (member.occupied)
              continue;
            member.occupied = true;
            std::strncpy(member.participant_id, participant_id,
                         sizeof(member.participant_id) - 1);
            member.type = ParticipantType::NAMO_NODE;
            member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
            member.membership = membership;
            member.leader_eligible = true;
            return true;
          }
          return false;
        };
        require(add_member(CANDIDATE_ID,
                           ParticipantMembership::QUARANTINED),
                "activation fault candidate slot missing");
        for (uint8_t index = 0; index < existing_remote_count; ++index) {
          char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
          std::snprintf(participant_id, sizeof(participant_id),
                        "namo-node-existing-%u",
                        static_cast<unsigned>(index));
          require(add_member(participant_id, ParticipantMembership::ACTIVE),
                  "activation fault existing member slot missing");
        }

        ParticipantEndpointCache endpoints{};
        const auto digest = site_configuration_digest(configuration);
        require(endpoints.observe_authenticated(CANDIDATE_ID, 0xC0A80401U,
                                                49191, NOW_MS) &&
                    endpoints.observe_floorplan_replica(
                        CANDIDATE_ID, configuration.authority_epoch,
                        configuration.generation, digest, 0, 0, 0, 0,
                        false),
                "activation fault candidate endpoint setup failed");
        char faulted_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
        for (uint8_t index = 0; index < existing_remote_count; ++index) {
          char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
          std::snprintf(participant_id, sizeof(participant_id),
                        "namo-node-existing-%u",
                        static_cast<unsigned>(index));
          if (index == fault_index && fault == PeerFault::OFFLINE) {
            std::strncpy(faulted_id, participant_id,
                         sizeof(faulted_id) - 1);
            continue;
          }
          require(endpoints.observe_authenticated(
                      participant_id, 0xC0A80500U + index, 49191, NOW_MS),
                  "activation fault endpoint authentication failed");

          uint32_t authority_epoch = configuration.authority_epoch;
          uint32_t generation = configuration.generation;
          uint64_t observed_digest = digest;
          if (index == fault_index) {
            std::strncpy(faulted_id, participant_id,
                         sizeof(faulted_id) - 1);
            switch (fault) {
              case PeerFault::BEHIND:
                --generation;
                break;
              case PeerFault::AHEAD:
                ++generation;
                observed_digest ^= 0x10U;
                break;
              case PeerFault::UNFENCED:
                authority_epoch = 0;
                observed_digest = 0;
                break;
              case PeerFault::DIVERGENT:
                observed_digest ^= 0x1U;
                break;
              case PeerFault::OFFLINE:
                break;
            }
          }
          require(endpoints.observe_floorplan_replica(
                      participant_id, authority_epoch, generation,
                      observed_digest, 0, 0, 0, 0, false),
                  "activation fault endpoint authority setup failed");
        }

        auto authority = evaluate_site_authority(
            &identity, &configuration, &endpoints, NOW_MS);
        const auto expected_state =
            fault == PeerFault::DIVERGENT
                ? SiteAuthorityState::CONFLICT
                : fault == PeerFault::OFFLINE
                      ? SiteAuthorityState::CONSISTENT
                      : SiteAuthorityState::CONVERGING;
        const auto expected_gate =
            fault == PeerFault::DIVERGENT
                ? SiteActivationAuthorityGateResult::CONFLICT
                : fault == PeerFault::OFFLINE
                      ? SiteActivationAuthorityGateResult::OK
                      : SiteActivationAuthorityGateResult::
                            CONTINUITY_UNAVAILABLE;
        require(authority.state == expected_state &&
                    evaluate_site_activation_authority_gate(
                        authority, configuration,
                        endpoints.find(CANDIDATE_ID), true) == expected_gate,
                "existing peer fault did not fence sequential activation");

        if (fault == PeerFault::OFFLINE) {
          require(endpoints.observe_authenticated(
                      faulted_id, 0xC0A80600U + fault_index, 49191, NOW_MS),
                  "offline peer repair authentication failed");
        }
        require(endpoints.observe_floorplan_replica(
                    faulted_id, configuration.authority_epoch,
                    configuration.generation, digest, 0, 0, 0, 0, false),
                "activation fault repair setup failed");
        authority = evaluate_site_authority(
            &identity, &configuration, &endpoints, NOW_MS);
        require(authority.consistent() &&
                    evaluate_site_activation_authority_gate(
                        authority, configuration,
                        endpoints.find(CANDIDATE_ID), true) ==
                        SiteActivationAuthorityGateResult::OK,
                "repaired peer did not reopen sequential activation");
      }
    }
  }
}

void topology_fence_allows_independent_floorplan_authority() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  auto endpoints = endpoint_cache(
      REMOTE_ID, configuration.authority_epoch,
      configuration.generation + 7,
      site_configuration_digest(configuration) ^ 0x55U);
  require(endpoints.observe_site_topology(
              REMOTE_ID, configuration.roster_revision,
              site_topology_digest(configuration)),
          "topology fence setup failed");
  const auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.spatially_consistent(),
          "independent control-plane generations must preserve a matching "
          "Site topology data plane");
}

void owner_handover_keeps_matching_spatial_topology_active() {
  const auto identity = local_identity();
  const auto configuration = configuration_with_remote();
  const auto handed_over = exact_owner_handover(configuration, REMOTE_ID);
  ParticipantEndpointCache endpoints{};
  observe_authority_endpoint(&endpoints, REMOTE_ID, handed_over,
                             0xC0A80102U);

  auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.reason == SiteAuthorityReason::LOCAL_BEHIND &&
              !authority.consistent() && authority.spatially_consistent(),
          "an exact owner-only handover closed a matching spatial topology");

  require(endpoints.observe_site_topology(
              REMOTE_ID, handed_over.roster_revision + 1U,
              site_topology_digest(handed_over) ^ 0x1U),
          "divergent topology fixture setup failed");
  authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              !authority.spatially_consistent(),
          "a changed roster topology remained eligible for spatial fusion");
}

void adjacent_roster_rollout_preserves_only_matching_spatial_contracts() {
  const auto identity = local_identity();
  auto configuration = configuration_with_remote();
  configuration.generation = 20;
  configuration.roster_revision = 12;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floorplan-main");
  configuration.floorplan.geometry_version = 7;
  configuration.floorplan.config_hash = 0x12345678U;
  configuration.floorplan.image_hash = 0x87654321U;
  SiteConfigurationMember *remote = nullptr;
  for (auto &member : configuration.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, REMOTE_ID) == 0) {
      remote = &member;
      break;
    }
  }
  require(remote != nullptr, "roster transition remote fixture is missing");
  remote->spatial_profile.committed = true;
  std::strcpy(remote->spatial_profile.space_id, "space-main");
  std::strcpy(remote->spatial_profile.group_id, "group-main");
  remote->spatial_profile.pose_version = 9;

  ParticipantEndpointCache endpoints{};
  require(endpoints.observe_authenticated(REMOTE_ID, 0xC0A80102U, 49191,
                                           NOW_MS) &&
              endpoints.observe_floorplan_replica(
                  REMOTE_ID, configuration.authority_epoch,
                  configuration.generation - 1U, 0x1111222233334444ULL,
                  configuration.floorplan.geometry_version,
                  configuration.floorplan.config_hash,
                  configuration.floorplan.image_hash,
                  remote->spatial_profile.pose_version, true) &&
              endpoints.observe_site_topology(
                  REMOTE_ID, configuration.roster_revision - 1U,
                  0x5555666677778888ULL) &&
              endpoints.observe_site_credentials(
                  REMOTE_ID, SiteHeartbeatEnvelope::CURRENT_WIRE_VERSION,
                  true),
          "roster transition endpoint fixture setup failed");

  auto authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              !authority.consistent() && authority.spatially_consistent(),
          "an adjacent roster rollout interrupted a matching spatial contract");

  require(endpoints.observe_floorplan_replica(
              REMOTE_ID, configuration.authority_epoch,
              configuration.generation - 1U, 0x1111222233334444ULL,
              configuration.floorplan.geometry_version,
              configuration.floorplan.config_hash,
              configuration.floorplan.image_hash,
              remote->spatial_profile.pose_version + 1U, true),
          "roster transition pose mismatch fixture setup failed");
  authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              !authority.spatially_consistent(),
          "a mismatched radar pose remained active during roster rollout");

  require(endpoints.observe_floorplan_replica(
              REMOTE_ID, configuration.authority_epoch,
              configuration.generation + 1U, 0x9999AAAABBBBCCCCULL,
              configuration.floorplan.geometry_version,
              configuration.floorplan.config_hash,
              configuration.floorplan.image_hash,
              remote->spatial_profile.pose_version, true) &&
              endpoints.observe_site_topology(
                  REMOTE_ID, configuration.roster_revision + 1U,
                  0xDDDDEEEEFFFF0001ULL),
          "forward roster transition fixture setup failed");
  authority =
      evaluate_site_authority(&identity, &configuration, &endpoints, NOW_MS);
  require(authority.state == SiteAuthorityState::CONVERGING &&
              authority.spatially_consistent(),
          "a forward adjacent roster rollout interrupted spatial fusion");
}

void replication_candidate_selects_owner_or_successor() {
  auto local_owner_configuration = configuration_with_remote();
  const auto handover =
      exact_owner_handover(local_owner_configuration, REMOTE_ID);
  ParticipantEndpointCache successor_endpoints{};
  observe_authority_endpoint(&successor_endpoints, REMOTE_ID, handover,
                             0xC0A80102U);
  auto selected = select_site_authority_replication_candidate(
      local_identity(), local_owner_configuration, &successor_endpoints,
      NOW_MS);
  require(selected.available &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0 &&
              selected.mode ==
                  SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
              selected.evidence.confirmed(),
          "a quorum-endorsed next-owner transition must use quorum rejoin");

  const auto owner = [] {
    PlatformIdentity value{};
    std::strcpy(value.site_id, "site-home");
    std::strcpy(value.node_id, REMOTE_ID);
    return value;
  }();
  SiteConfigurationSnapshot remote_owner_configuration{};
  require(initialize_site_configuration(owner, false,
                                        &remote_owner_configuration) ==
              SiteConfigurationResult::OK,
          "remote owner configuration bootstrap failed");
  for (auto &member : remote_owner_configuration.members) {
    if (member.occupied)
      continue;
    member.occupied = true;
    std::strcpy(member.participant_id, LOCAL_ID);
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    break;
  }
  const auto owner_endpoints = endpoint_cache(
      REMOTE_ID, remote_owner_configuration.authority_epoch,
      remote_owner_configuration.generation + 1,
      site_configuration_digest(remote_owner_configuration) ^ 0x20U);
  selected = select_site_authority_replication_candidate(
      local_identity(), remote_owner_configuration, &owner_endpoints, NOW_MS);
  require(selected.available &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0 &&
              selected.mode == SiteConfigurationReplicationMode::STRICT,
          "newer current owner must be selected without handover authority");

  const auto current_endpoints = endpoint_cache(
      REMOTE_ID, remote_owner_configuration.authority_epoch,
      remote_owner_configuration.generation,
      site_configuration_digest(remote_owner_configuration));
  selected = select_site_authority_replication_candidate(
      local_identity(), remote_owner_configuration, &current_endpoints,
      NOW_MS);
  require(!selected.available,
          "an equal revision must not schedule automatic replication");
}

void exact_owner_handover_propagates_from_one_replica_through_seven_nodes() {
  constexpr const char *IDS[] = {
      "namo-node-owner", REMOTE_ID, LOCAL_ID, "namo-node-c",
      "namo-node-d", "namo-node-e", "namo-node-f"};
  for (std::size_t node_count = 2; node_count <= 7; ++node_count) {
    PlatformIdentity owner{};
    std::strcpy(owner.site_id, "site-home");
    std::strcpy(owner.node_id, IDS[0]);
    SiteConfigurationSnapshot current{};
    require(initialize_site_configuration(owner, false, &current) ==
                SiteConfigurationResult::OK,
            "multi-node handover configuration bootstrap failed");
    for (std::size_t index = 1; index < node_count; ++index)
      add_active_member(&current, IDS[index]);
    current.roster_committed = true;
    current.roster_revision = 20;
    current.generation = 30;
    current.authority_epoch = 4;
    const auto handover = exact_owner_handover(current, IDS[1]);

    ParticipantEndpointCache endpoints{};
    observe_authority_endpoint(&endpoints, IDS[1], handover,
                               0xC0A80102U);
    PlatformIdentity receiver = owner;
    if (node_count > 2)
      std::strcpy(receiver.node_id, IDS[2]);
    const auto selected = select_site_authority_replication_candidate(
        receiver, current, &endpoints, NOW_MS);
    require(selected.available &&
                std::strcmp(selected.source_node_id, IDS[1]) == 0 &&
                selected.authority_epoch == handover.authority_epoch &&
                selected.generation == handover.generation &&
                selected.mode ==
                    (node_count == 2
                         ? SiteConfigurationReplicationMode::
                               QUORUM_AUTHORITY_REJOIN
                         : SiteConfigurationReplicationMode::
                               AUTOMATIC_OWNER_HANDOVER),
            "a lone exact handover replica did not propagate in a 2-7 Node Site");
  }
}

void online_old_owner_must_endorse_an_exact_handover() {
  PlatformIdentity owner{};
  std::strcpy(owner.site_id, "site-home");
  std::strcpy(owner.node_id, "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "owner endorsement configuration bootstrap failed");
  add_active_member(&current, REMOTE_ID);
  add_active_member(&current, LOCAL_ID);
  add_active_member(&current, "namo-node-c");
  add_active_member(&current, "namo-node-d");
  current.roster_committed = true;
  current.roster_revision = 20;
  current.generation = 30;
  current.authority_epoch = 4;
  const auto handover = exact_owner_handover(current, REMOTE_ID);

  ParticipantEndpointCache endpoints{};
  observe_authority_endpoint(&endpoints, REMOTE_ID, handover, 0xC0A80102U);
  auto selected = select_site_authority_replication_candidate(
      owner, current, &endpoints, NOW_MS);
  require(selected.available &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0 &&
              selected.mode ==
                  SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
          "a returning old owner did not accept its exact successor revision");

  observe_authority_endpoint(&endpoints, owner.node_id, current,
                             0xC0A80103U);
  selected = select_site_authority_replication_candidate(
      local_identity(), current, &endpoints, NOW_MS);
  require(!selected.available,
          "an online old owner allowed an unendorsed handover");

  observe_authority_endpoint(&endpoints, owner.node_id, handover,
                             0xC0A80103U);
  selected = select_site_authority_replication_candidate(
      local_identity(), current, &endpoints, NOW_MS);
  require(selected.available &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0 &&
              selected.mode ==
                  SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER,
          "the old owner's exact handover endorsement did not restore convergence");
}

void competing_exact_owner_handovers_fail_closed() {
  PlatformIdentity owner{};
  std::strcpy(owner.site_id, "site-home");
  std::strcpy(owner.node_id, "namo-node-owner");
  SiteConfigurationSnapshot current{};
  require(initialize_site_configuration(owner, false, &current) ==
              SiteConfigurationResult::OK,
          "competing handover configuration bootstrap failed");
  add_active_member(&current, REMOTE_ID);
  add_active_member(&current, "namo-node-other");
  add_active_member(&current, LOCAL_ID);
  current.roster_committed = true;
  current.roster_revision = 20;
  current.generation = 30;
  current.authority_epoch = 4;
  const auto first = exact_owner_handover(current, REMOTE_ID);
  const auto second = exact_owner_handover(current, "namo-node-other");

  ParticipantEndpointCache endpoints{};
  observe_authority_endpoint(&endpoints, REMOTE_ID, first, 0xC0A80102U);
  observe_authority_endpoint(&endpoints, "namo-node-other", second,
                             0xC0A80103U);
  require(!select_site_authority_replication_candidate(
               local_identity(), current, &endpoints, NOW_MS)
               .available,
          "competing exact next-owner revisions did not fail closed");
}

void enrollment_candidate_survives_reboot_without_relaxing_normal_authority() {
  PlatformIdentity joiner{};
  std::strcpy(joiner.site_id, "site-home");
  std::strcpy(joiner.node_id, "namo-node-joiner");
  SiteConfigurationSnapshot local{};
  require(initialize_site_configuration(joiner, true, &local) ==
              SiteConfigurationResult::OK,
          "enrollment candidate local fixture failed");
  ParticipantRegistry participants{};
  require(participants.discover(REMOTE_ID, ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              participants.classify(
                  REMOTE_ID, ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK,
          "enrollment candidate participant fixture failed");
  ParticipantCredentialRegistry credentials{};
  std::array<uint8_t, 32> key{};
  key[0] = 1;
  require(credentials.set(REMOTE_ID, 99, 1, key.data(), true) ==
              ParticipantCredentialResult::OK,
          "enrollment candidate credential fixture failed");
  auto endpoints = endpoint_cache(REMOTE_ID, 5, 74, 0x1234);
  const auto selected = select_enrollment_replication_candidate(
      joiner, local, participants, credentials, &endpoints, NOW_MS);
  require(selected.available && selected.authority_epoch == 5 &&
              selected.generation == 74 &&
              selected.mode ==
                  SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0,
          "rebooted enrollment did not schedule its exact verified owner");

  const auto *source_endpoint = endpoints.find(REMOTE_ID);
  require(!automatic_enrollment_floorplan_replication_allowed(
              selected, source_endpoint, false, 0, 0),
          "an empty joining device allowed automatic Floorplan replacement");
  require(endpoints.observe_floorplan_replica(
              REMOTE_ID, 5, 74, 0x1234, 3, 101, 202, 1, true),
          "enrollment Floorplan endpoint fixture failed");
  const auto matching_floorplan_candidate =
      select_enrollment_replication_candidate(
          joiner, local, participants, credentials, &endpoints, NOW_MS);
  source_endpoint = endpoints.find(REMOTE_ID);
  require(automatic_enrollment_floorplan_replication_allowed(
              matching_floorplan_candidate, source_endpoint, true, 101, 202),
          "an exact stored Floorplan did not allow enrollment convergence");
  require(!automatic_enrollment_floorplan_replication_allowed(
              matching_floorplan_candidate, source_endpoint, true, 101, 999),
          "a different stored Floorplan allowed automatic replacement");

  auto disabled_credentials = credentials;
  require(disabled_credentials.set_enabled(REMOTE_ID, false) ==
                  ParticipantCredentialResult::OK &&
              !select_enrollment_replication_candidate(
                   joiner, local, participants, disabled_credentials,
                   &endpoints, NOW_MS)
                   .available,
          "a disabled pairing credential scheduled enrollment replication");

  auto stale_endpoints = endpoint_cache(REMOTE_ID, 0, 0, 0);
  require(!select_enrollment_replication_candidate(
               joiner, local, participants, credentials, &stale_endpoints,
               NOW_MS)
               .available,
          "an unfenced enrollment endpoint must not schedule replication");

  auto converged = local;
  std::strcpy(converged.configuration_owner_node_id, REMOTE_ID);
  converged.roster_committed = true;
  require(!select_enrollment_replication_candidate(
               joiner, converged, participants, credentials, &endpoints,
               NOW_MS)
               .available,
          "a converged owner must permanently close enrollment scheduling");

  auto ambiguous = participants;
  require(ambiguous.discover("namo-node-other", ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              !select_enrollment_replication_candidate(
                   joiner, local, ambiguous, credentials, &endpoints, NOW_MS)
                   .available,
          "multiple non-revoked peers must not select an enrollment source");

}

void canonical_directory_does_not_mask_newer_owner_epoch() {
  auto configuration = configuration_with_remote();
  configuration.authority_epoch = 2;
  configuration.generation = 66;
  configuration.roster_revision = 54;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "fp-home");
  configuration.floorplan.geometry_version = 1;
  configuration.floorplan.config_hash = 101;
  configuration.floorplan.image_hash = 202;

  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, configuration.site_id);
  directory.generation = 8;
  directory.topology_revision = configuration.roster_revision;
  directory.committed = true;
  auto &floorplan = directory.floorplans[0];
  floorplan.occupied = true;
  floorplan.committed = true;
  std::strcpy(floorplan.floorplan_id,
              configuration.floorplan.floorplan_id);
  std::strcpy(floorplan.owner_node_id, LOCAL_ID);
  floorplan.owner_generation = configuration.generation;
  floorplan.geometry_version = configuration.floorplan.geometry_version;
  floorplan.config_hash = configuration.floorplan.config_hash;
  floorplan.image_hash = configuration.floorplan.image_hash;
  require(valid_site_directory(directory),
          "canonical handover directory fixture is invalid");

  auto endpoints = endpoint_cache(
      REMOTE_ID, 3, 67, site_configuration_digest(configuration) ^ 0x40U);
  require(endpoints.observe_site_topology(
              REMOTE_ID, configuration.roster_revision + 1, 0x12345678U),
          "new owner topology fence setup failed");

  const auto selected = select_site_replication_candidate(
      local_identity(), configuration, &directory, &endpoints, NOW_MS);
  require(selected.available &&
              std::strcmp(selected.source_node_id, REMOTE_ID) == 0 &&
              selected.authority_epoch == 3 &&
              selected.generation == 67 &&
              selected.mode ==
                      SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
              selected.evidence.confirmed() &&
              !selected.route.manifest_bound,
          "canonical directory masked the newer owner authority epoch");
}

void quorum_rejoin_selects_any_exact_replica_and_rejects_conflicts() {
  auto current = configuration_with_remote();
  add_active_member(&current, "namo-node-c");
  add_active_member(&current, "namo-node-d");
  add_active_member(&current, "namo-node-e");
  current.roster_committed = true;
  current.roster_revision = 10;
  current.generation = 20;
  current.authority_epoch = 3;

  auto remote = current;
  std::strcpy(remote.configuration_owner_node_id, "namo-node-d");
  remote.authority_epoch = 4;
  remote.generation = 21;
  remote.roster_revision = 11;

  ParticipantEndpointCache endpoints{};
  observe_authority_endpoint(&endpoints, REMOTE_ID, remote, 0xC0A80102U);
  observe_authority_endpoint(&endpoints, "namo-node-c", remote,
                             0xC0A80103U);
  observe_authority_endpoint(&endpoints, "namo-node-d", remote,
                             0xC0A80104U);

  auto selected = select_site_authority_replication_candidate(
      local_identity(), current, &endpoints, NOW_MS);
  require(selected.available &&
              selected.mode ==
                  SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
              selected.evidence.confirmed() &&
              selected.evidence.endorsements == 3 &&
              selected.evidence.quorum_required == 3 &&
              std::strcmp(selected.source_node_id, "namo-node-c") == 0,
          "quorum rejoin must select a deterministic exact replica");

  auto conflicting = remote;
  conflicting.generation++;
  conflicting.roster_revision++;
  observe_authority_endpoint(&endpoints, "namo-node-e", conflicting,
                             0xC0A80105U);
  selected = select_site_authority_replication_candidate(
      local_identity(), current, &endpoints, NOW_MS);
  require(selected.available && selected.evidence.endorsements == 3 &&
              selected.evidence.authority_epoch == remote.authority_epoch &&
              selected.evidence.generation == remote.generation,
          "a lone conflicting advertisement must not replace quorum evidence");

  ParticipantEndpointCache insufficient{};
  observe_authority_endpoint(&insufficient, REMOTE_ID, remote, 0xC0A80102U);
  observe_authority_endpoint(&insufficient, "namo-node-c", remote,
                             0xC0A80103U);
  require(!select_site_authority_replication_candidate(
               local_identity(), current, &insufficient, NOW_MS)
               .available,
          "five-member authority rejoin must fail closed below quorum");
}

void multiple_stale_owners_rejoin_latest_authority_from_two_to_seven() {
  constexpr const char *IDS[] = {
      LOCAL_ID, REMOTE_ID, "namo-node-c", "namo-node-d",
      "namo-node-e", "namo-node-f", "namo-node-g"};
  for (std::size_t member_count = 2; member_count <= 7; ++member_count) {
    PlatformIdentity first{};
    std::strcpy(first.site_id, "site-home");
    std::strcpy(first.node_id, IDS[0]);
    SiteConfigurationSnapshot stale{};
    require(initialize_site_configuration(first, false, &stale) ==
                SiteConfigurationResult::OK,
            "multi-owner stale configuration setup failed");
    for (std::size_t index = 1; index < member_count; ++index)
      add_active_member(&stale, IDS[index]);
    stale.roster_committed = true;
    stale.roster_revision = 10;
    stale.authority_epoch = 1;
    stale.generation = 20;

    auto latest = stale;
    std::strcpy(latest.configuration_owner_node_id, IDS[member_count - 1]);
    latest.authority_epoch = static_cast<uint32_t>(member_count + 3);
    latest.generation = static_cast<uint32_t>(member_count + 30);
    latest.roster_revision = static_cast<uint32_t>(member_count + 20);

    const uint8_t required = site_authority_rejoin_quorum_required(stale);
    ParticipantEndpointCache endpoints{};
    for (std::size_t index = member_count - required;
         index < member_count; ++index) {
      observe_authority_endpoint(
          &endpoints, IDS[index], latest,
          static_cast<uint32_t>(0xC0A80100U + index + 1U));
    }

    const std::size_t stale_owner_count =
        member_count > required ? member_count - required : 1;
    for (std::size_t owner_index = 0; owner_index < stale_owner_count;
         ++owner_index) {
      PlatformIdentity returning = first;
      std::strcpy(returning.node_id, IDS[owner_index]);
      auto owner_view = stale;
      std::strcpy(owner_view.configuration_owner_node_id, IDS[owner_index]);
      owner_view.authority_epoch = static_cast<uint32_t>(owner_index + 1);
      owner_view.generation = static_cast<uint32_t>(20 + owner_index);
      const auto selected = select_site_authority_replication_candidate(
          returning, owner_view, &endpoints, NOW_MS);
      require(selected.available && selected.evidence.confirmed() &&
                  selected.evidence.authority_epoch == latest.authority_epoch &&
                  selected.evidence.generation == latest.generation &&
                  selected.evidence.endorsements >= required,
              "every stale owner must select the latest quorum authority");
    }
  }
}

void quorum_repairs_equal_revision_owner_fork_after_successor_loss() {
  auto abandoned = configuration_with_remote();
  add_active_member(&abandoned, "namo-node-c");
  add_active_member(&abandoned, "namo-node-d");
  add_active_member(&abandoned, "namo-node-e");
  abandoned.roster_committed = true;
  abandoned.roster_revision = 18;
  abandoned.authority_epoch = 5;
  abandoned.generation = 31;

  auto surviving = abandoned;
  std::strcpy(surviving.configuration_owner_node_id, "namo-node-d");

  ParticipantEndpointCache quorum{};
  observe_authority_endpoint(&quorum, REMOTE_ID, surviving, 0xC0A80102U);
  observe_authority_endpoint(&quorum, "namo-node-c", surviving,
                             0xC0A80103U);
  observe_authority_endpoint(&quorum, "namo-node-d", surviving,
                             0xC0A80104U);
  const auto selected = select_site_authority_replication_candidate(
      local_identity(), abandoned, &quorum, NOW_MS);
  require(selected.available &&
              selected.mode ==
                  SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
              selected.authority_epoch == abandoned.authority_epoch &&
              selected.generation == abandoned.generation &&
              selected.evidence.confirmed() &&
              selected.evidence.endorsements == 3,
          "a returning unpropagated successor did not select the surviving quorum owner");

  ParticipantEndpointCache minority{};
  observe_authority_endpoint(&minority, REMOTE_ID, surviving,
                             0xC0A80102U);
  observe_authority_endpoint(&minority, "namo-node-c", surviving,
                             0xC0A80103U);
  require(!select_site_authority_replication_candidate(
               local_identity(), abandoned, &minority, NOW_MS)
               .available,
          "an equal-revision owner fork was repaired below quorum");

  auto two_node = configuration_with_remote();
  two_node.roster_committed = true;
  two_node.authority_epoch = 5;
  two_node.generation = 31;
  auto two_node_fork = two_node;
  std::strcpy(two_node_fork.configuration_owner_node_id, REMOTE_ID);
  ParticipantEndpointCache one_peer{};
  observe_authority_endpoint(&one_peer, REMOTE_ID, two_node_fork,
                             0xC0A80102U);
  require(!select_site_authority_replication_candidate(
               local_identity(), two_node, &one_peer, NOW_MS)
               .available,
          "a two-node equal-revision conflict must fail closed");
}

void replication_tracker_serializes_start_and_completion() {
  SiteAuthorityReplicationTracker tracker{};
  tracker.observe_candidate(candidate(2, 3), NOW_MS);
  require(tracker.pending_authority_epoch() == 2 &&
              tracker.pending_generation() == 3 &&
              tracker.wait_remaining_ms(NOW_MS) == 2500 && tracker.busy(),
          "candidate must enter the deterministic pending slot");
  require(!tracker.start_due(NOW_MS + 2499, SiteReplicationState::IDLE) &&
              tracker.start_due(NOW_MS + 2500,
                                SiteReplicationState::IDLE) &&
              !tracker.start_due(NOW_MS + 2500,
                                 SiteReplicationState::CONFIG_PAYLOAD),
          "start must wait for both the slot and an idle transport");
  tracker.complete_start(SiteReplicationResult::OK, NOW_MS + 2500);
  require(tracker.pending_generation() == 0 &&
              tracker.active_generation() == 3 && tracker.busy(),
          "successful start must move pending revision to active");

  auto local = configuration_with_remote();
  local.authority_epoch = 2;
  local.generation = 3;
  tracker.observe_transport(local, SiteReplicationState::COMPLETE,
                            NOW_MS + 2600);
  require(!tracker.busy() && tracker.retry_count() == 0,
          "completed local convergence must clear active state");
}

void replication_tracker_retries_then_cools_down_without_sticking() {
  SiteAuthorityReplicationTracker tracker{};
  auto local = configuration_with_remote();
  local.authority_epoch = 1;
  local.generation = 1;
  uint32_t now_ms = NOW_MS;
  tracker.observe_candidate(candidate(2, 1, 0), now_ms);
  for (uint8_t attempt = 0;
       attempt <= SITE_REPLICATION_MAX_SESSION_RETRIES; ++attempt) {
    require(tracker.start_due(now_ms, SiteReplicationState::IDLE),
            "scheduled retry must become startable");
    tracker.complete_start(SiteReplicationResult::OK, now_ms);
    tracker.observe_transport(local, SiteReplicationState::FAILED, now_ms);
    if (attempt < SITE_REPLICATION_MAX_SESSION_RETRIES) {
      require(tracker.pending_generation() == 1 &&
                  tracker.retry_count() == attempt + 1,
              "failed session must schedule bounded retry");
      now_ms += tracker.wait_remaining_ms(now_ms);
    }
  }
  require(tracker.pending_generation() == 1 &&
              tracker.active_generation() == 0 &&
              tracker.blocked_generation() == 0 && tracker.busy() &&
              tracker.retry_count() == 0,
          "retry exhaustion must cool down without permanently blocking the revision");
  tracker.observe_candidate(candidate(2, 1, 0), now_ms);
  require(tracker.busy() && !tracker.start_due(now_ms,
                                                SiteReplicationState::IDLE),
          "cooldown must suppress an immediate retry storm");
  now_ms += SiteAuthorityReplicationTracker::FAILURE_COOLDOWN_MS;
  require(tracker.start_due(now_ms, SiteReplicationState::IDLE),
          "the same revision must become retryable after cooldown");
  tracker.observe_candidate(candidate(2, 2, 0), now_ms);
  require(tracker.pending_generation() == 2,
          "newer revision must replace cooling work immediately");
}

void replication_tracker_preserves_newer_pending_work() {
  SiteAuthorityReplicationTracker tracker{};
  auto local = configuration_with_remote();
  local.authority_epoch = 1;
  local.generation = 1;
  tracker.observe_candidate(candidate(2, 1, 0), NOW_MS);
  tracker.complete_start(SiteReplicationResult::OK, NOW_MS);
  tracker.observe_candidate(candidate(2, 2, 100), NOW_MS);
  tracker.observe_transport(local, SiteReplicationState::FAILED, NOW_MS);
  require(tracker.pending_generation() == 2 && tracker.retry_count() == 0,
          "newer pending revision must survive an older active failure");

  tracker.complete_start(SiteReplicationResult::OK, NOW_MS + 100);
  tracker.observe_transport(local, SiteReplicationState::CONFLICT,
                            NOW_MS + 101);
  require(!tracker.busy() && tracker.blocked_generation() == 2,
          "transport conflict must block the active revision");
  tracker.reset();
  require(!tracker.busy() && tracker.blocked_generation() == 0 &&
              tracker.source_node_id()[0] == '\0',
          "reset must remove all scheduler state");
}

void replication_tracker_rotates_replica_for_the_same_authority_revision() {
  SiteAuthorityReplicationTracker tracker{};
  auto first = candidate(7, 40, 2500);
  first.mode = SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN;
  std::strcpy(first.source_node_id, "namo-node-a");
  first.evidence.authority_epoch = 7;
  first.evidence.generation = 40;
  first.evidence.configuration_digest = 10;
  first.evidence.topology_revision = 9;
  first.evidence.topology_digest = 11;
  first.evidence.endorsements = 3;
  first.evidence.quorum_required = 3;
  tracker.observe_candidate(first, NOW_MS);
  require(std::strcmp(tracker.source_node_id(), "namo-node-a") == 0,
          "first quorum replica was not scheduled");

  auto replacement = first;
  std::strcpy(replacement.source_node_id, "namo-node-b");
  replacement.slot_delay_ms = 0;
  tracker.observe_candidate(replacement, NOW_MS + 1);
  require(std::strcmp(tracker.source_node_id(), "namo-node-b") == 0 &&
              tracker.pending_authority_epoch() == 7 &&
              tracker.pending_generation() == 40 &&
              tracker.start_due(NOW_MS + 1, SiteReplicationState::IDLE),
          "same-revision replica replacement remained pinned to a failed source");
}

void replication_conflict_blocks_only_the_exact_candidate() {
  SiteAuthorityReplicationTracker tracker{};
  auto conflicting = candidate(8, 51, 0);
  conflicting.mode =
      SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN;
  std::strcpy(conflicting.source_node_id, "namo-node-a");
  conflicting.configuration_digest = 0x1111;
  tracker.observe_candidate(conflicting, NOW_MS);
  tracker.complete_start(SiteReplicationResult::OK, NOW_MS);

  auto local = configuration_with_remote();
  tracker.observe_transport(local, SiteReplicationState::CONFLICT,
                            NOW_MS + 1);
  require(!tracker.busy() && tracker.blocked_generation() == 51,
          "transport conflict did not retain its exact blocked candidate");
  tracker.observe_candidate(conflicting, NOW_MS + 2);
  require(!tracker.busy(),
          "the exact conflicting candidate was immediately retried");

  auto quorum_alternative = conflicting;
  std::strcpy(quorum_alternative.source_node_id, "namo-node-b");
  tracker.observe_candidate(quorum_alternative, NOW_MS + 3);
  require(tracker.busy() && tracker.pending_generation() == 51 &&
              tracker.blocked_generation() == 0 &&
              std::strcmp(tracker.source_node_id(), "namo-node-b") == 0,
          "an alternate quorum source at the same revision remained permanently blocked");
}

}  // namespace

void run_site_authority_tests() {
  unavailable_inputs_fail_closed();
  standalone_and_offline_members_are_consistent();
  matching_online_member_is_consistent();
  revision_lag_is_convergence_not_conflict();
  unfenced_peer_is_converging();
  equal_revision_digest_divergence_is_conflict();
  activation_gate_distinguishes_convergence_from_conflict();
  third_through_seventh_member_wait_for_existing_peers();
  sequential_activation_fault_position_matrix();
  topology_fence_allows_independent_floorplan_authority();
  owner_handover_keeps_matching_spatial_topology_active();
  adjacent_roster_rollout_preserves_only_matching_spatial_contracts();
  replication_candidate_selects_owner_or_successor();
  exact_owner_handover_propagates_from_one_replica_through_seven_nodes();
  online_old_owner_must_endorse_an_exact_handover();
  competing_exact_owner_handovers_fail_closed();
  enrollment_candidate_survives_reboot_without_relaxing_normal_authority();
  canonical_directory_does_not_mask_newer_owner_epoch();
  quorum_rejoin_selects_any_exact_replica_and_rejects_conflicts();
  multiple_stale_owners_rejoin_latest_authority_from_two_to_seven();
  quorum_repairs_equal_revision_owner_fork_after_successor_loss();
  replication_tracker_serializes_start_and_completion();
  replication_tracker_retries_then_cools_down_without_sticking();
  replication_tracker_preserves_newer_pending_work();
  replication_tracker_rotates_replica_for_the_same_authority_revision();
  replication_conflict_blocks_only_the_exact_candidate();
}
