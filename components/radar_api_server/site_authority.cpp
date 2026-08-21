#include "site_authority.h"

#include <cstring>
#include <limits>

namespace esphome {
namespace radar_api_server {

namespace {

bool endpoint_spatial_contract_matches(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration,
    const SiteConfigurationMember &member) {
  return endpoint != nullptr && configuration.floorplan.committed &&
         member.spatial_profile.committed &&
         endpoint->floorplan_replica_ready &&
         endpoint->floorplan_geometry_version ==
             configuration.floorplan.geometry_version &&
         endpoint->floorplan_config_hash ==
             configuration.floorplan.config_hash &&
         endpoint->floorplan_image_hash == configuration.floorplan.image_hash &&
         endpoint->radar_pose_version == member.spatial_profile.pose_version;
}

}  // namespace

SiteAuthoritySnapshot evaluate_site_authority(
    const PlatformIdentity *identity,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms) {
  SiteAuthoritySnapshot snapshot{};
  snapshot.evaluated = true;
  snapshot.evaluated_at_ms = now_ms;
  if (identity == nullptr || configuration == nullptr || endpoints == nullptr ||
      identity->node_id[0] == '\0' || configuration->site_id[0] == '\0' ||
      configuration->authority_epoch == 0 || configuration->generation == 0 ||
      std::strcmp(identity->site_id, configuration->site_id) != 0) {
    return snapshot;
  }

  snapshot.local_authority_epoch = configuration->authority_epoch;
  snapshot.local_generation = configuration->generation;
  snapshot.local_digest = site_configuration_digest(*configuration);
  const uint32_t local_topology_revision = configuration->roster_revision;
  const uint64_t local_topology_digest = site_topology_digest(*configuration);
  snapshot.reason = SiteAuthorityReason::NONE;
  snapshot.spatial_consistent = true;
  bool converging = false;
  bool conflicting = false;

  for (const auto &member : configuration->members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        std::strcmp(member.participant_id, identity->node_id) == 0)
      continue;

    const auto *endpoint = endpoints->find(member.participant_id);
    if (!ParticipantEndpointCache::is_online(endpoint, now_ms))
      continue;
    const bool topology_fenced = endpoint->site_topology_revision != 0 &&
                                 endpoint->site_topology_digest != 0;
    bool endpoint_topology_matches = true;
    if (topology_fenced) {
      if (endpoint->site_topology_revision != local_topology_revision) {
        // Control-plane writes remain converging; only an unchanged spatial
        // contract keeps the already-running fusion data plane available.
        const bool bounded_roster_transition =
            participant_endpoint_is_bounded_roster_transition(
                endpoint, *configuration, member.participant_id,
                endpoint->latest_heartbeat_wire_version);
        if (!bounded_roster_transition ||
            !endpoint_spatial_contract_matches(endpoint, *configuration,
                                               member))
          snapshot.spatial_consistent = false;
        endpoint_topology_matches = false;
        converging = true;
        if (snapshot.reason == SiteAuthorityReason::NONE)
          snapshot.reason =
              endpoint->site_topology_revision > local_topology_revision
                  ? SiteAuthorityReason::LOCAL_BEHIND
                  : SiteAuthorityReason::PEER_BEHIND;
      } else if (endpoint->site_topology_digest != local_topology_digest) {
        snapshot.spatial_consistent = false;
        endpoint_topology_matches = false;
        conflicting = true;
        snapshot.reason = SiteAuthorityReason::DIVERGENT_DIGEST;
      }
      if (!endpoint_topology_matches)
        continue;
    }
    if (endpoint->site_configuration_authority_epoch == 0 ||
        endpoint->site_configuration_generation == 0 ||
        endpoint->site_configuration_digest == 0) {
      if (!topology_fenced)
        snapshot.spatial_consistent = false;
      converging = true;
      if (snapshot.reason == SiteAuthorityReason::NONE)
        snapshot.reason = SiteAuthorityReason::UNFENCED_PEER;
      continue;
    }

    const auto revision = compare_site_configuration_revision(
        endpoint->site_configuration_authority_epoch,
        endpoint->site_configuration_generation,
        configuration->authority_epoch, configuration->generation);
    if (revision == SiteConfigurationRevisionOrder::NEWER) {
      if (!topology_fenced)
        snapshot.spatial_consistent = false;
      converging = true;
      if (snapshot.reason == SiteAuthorityReason::NONE)
        snapshot.reason = SiteAuthorityReason::LOCAL_BEHIND;
      continue;
    }
    if (revision == SiteConfigurationRevisionOrder::OLDER) {
      if (!topology_fenced)
        snapshot.spatial_consistent = false;
      converging = true;
      if (snapshot.reason == SiteAuthorityReason::NONE)
        snapshot.reason = SiteAuthorityReason::PEER_BEHIND;
      continue;
    }
    if (endpoint->site_configuration_digest != snapshot.local_digest) {
      if (!topology_fenced)
        snapshot.spatial_consistent = false;
      conflicting = true;
      snapshot.reason = SiteAuthorityReason::DIVERGENT_DIGEST;
      continue;
    }
  }

  if (conflicting) {
    snapshot.state = SiteAuthorityState::CONFLICT;
  } else if (converging) {
    snapshot.state = SiteAuthorityState::CONVERGING;
  } else if (configuration->active_member_count() <= 1) {
    snapshot.state = SiteAuthorityState::STANDALONE;
  } else {
    snapshot.state = SiteAuthorityState::CONSISTENT;
  }
  return snapshot;
}

SiteActivationAuthorityGateResult evaluate_site_activation_authority_gate(
    const SiteAuthoritySnapshot &authority,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpoint *endpoint, bool endpoint_online) {
  if (authority.state == SiteAuthorityState::CONFLICT)
    return SiteActivationAuthorityGateResult::CONFLICT;
  if (!authority.consistent())
    return SiteActivationAuthorityGateResult::CONTINUITY_UNAVAILABLE;
  if (!endpoint_online || endpoint == nullptr)
    return SiteActivationAuthorityGateResult::OK;
  if (participant_endpoint_matches_configuration(endpoint, configuration))
    return SiteActivationAuthorityGateResult::OK;

  const bool same_revision =
      endpoint->site_configuration_authority_epoch ==
          configuration.authority_epoch &&
      endpoint->site_configuration_generation == configuration.generation;
  if (same_revision && endpoint->site_configuration_digest != 0 &&
      endpoint->site_configuration_digest !=
          site_configuration_digest(configuration))
    return SiteActivationAuthorityGateResult::CONFLICT;
  return SiteActivationAuthorityGateResult::CONTINUITY_UNAVAILABLE;
}

SiteAuthorityReplicationCandidate select_site_authority_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms) {
  SiteAuthorityReplicationCandidate candidate{};
  if (identity.node_id[0] == '\0' || endpoints == nullptr)
    return candidate;

  const auto endpoint_is_newer =
      [&configuration, now_ms](const ParticipantEndpoint *endpoint) {
        return ParticipantEndpointCache::is_online(endpoint, now_ms) &&
               endpoint->site_credentials_complete &&
               compare_site_configuration_revision(
                   endpoint->site_configuration_authority_epoch,
                   endpoint->site_configuration_generation,
                   configuration.authority_epoch,
                   configuration.generation) ==
                   SiteConfigurationRevisionOrder::NEWER;
      };
  const auto endpoint_can_restore_quorum_authority =
      [&configuration, now_ms](const ParticipantEndpoint *endpoint) {
        if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
            !endpoint->site_credentials_complete)
          return false;
        const auto order = compare_site_configuration_revision(
            endpoint->site_configuration_authority_epoch,
            endpoint->site_configuration_generation,
            configuration.authority_epoch, configuration.generation);
        if (order == SiteConfigurationRevisionOrder::NEWER)
          return true;
        return order == SiteConfigurationRevisionOrder::SAME &&
               configuration.active_member_count() > 2 &&
               endpoint->site_configuration_digest != 0 &&
               endpoint->site_configuration_digest !=
                   site_configuration_digest(configuration) &&
               endpoint->site_topology_revision ==
                   configuration.roster_revision &&
               endpoint->site_topology_digest ==
                   site_topology_digest(configuration);
      };

  const char *source = configuration.configuration_owner_node_id;
  const ParticipantEndpoint *source_endpoint = nullptr;
  if (source[0] != '\0' && std::strcmp(source, identity.node_id) != 0)
    source_endpoint = endpoints->find(source);

  const bool current_owner_same_epoch_newer =
      endpoint_is_newer(source_endpoint) &&
      source_endpoint->site_configuration_authority_epoch ==
          configuration.authority_epoch;
  if (!current_owner_same_epoch_newer) {
    const auto exact_automatic_handover =
        [&configuration, &endpoint_is_newer](
            const SiteConfigurationMember &member,
            const ParticipantEndpoint *endpoint) {
          if (!endpoint_is_newer(endpoint) || !member.occupied ||
              member.membership != ParticipantMembership::ACTIVE ||
              member.type != ParticipantType::NAMO_NODE ||
              !member.leader_eligible ||
              std::strcmp(member.participant_id,
                          configuration.configuration_owner_node_id) == 0)
            return false;
          return automatic_configuration_handover_bootstrap_allowed(
              configuration, configuration.configuration_owner_node_id,
              member.participant_id,
              endpoint->site_configuration_authority_epoch,
              endpoint->site_configuration_generation,
              endpoint->site_configuration_digest,
              endpoint->site_topology_revision,
              endpoint->site_topology_digest);
        };

    const ParticipantEndpoint *handover_endpoint = nullptr;
    const char *handover_source = nullptr;
    for (const auto &member : configuration.members) {
      if (!member.occupied ||
          std::strcmp(member.participant_id, identity.node_id) == 0)
        continue;
      const auto *endpoint = endpoints->find(member.participant_id);
      if (!exact_automatic_handover(member, endpoint))
        continue;
      if (!automatic_handover_authority_available(
              source_endpoint, now_ms,
              endpoint->site_configuration_authority_epoch,
              endpoint->site_configuration_generation,
              endpoint->site_configuration_digest,
              endpoint->site_topology_revision,
              endpoint->site_topology_digest))
        continue;
      if (handover_source != nullptr) {
        // Two different exact next owners are a fork, not a valid handover.
        return candidate;
      }
      handover_source = member.participant_id;
      handover_endpoint = endpoint;
    }
    if (handover_source != nullptr && handover_endpoint != nullptr) {
      uint8_t matching_replicas = 0;
      for (const auto &member : configuration.members) {
        if (!member.occupied ||
            member.membership != ParticipantMembership::ACTIVE ||
            member.type != ParticipantType::NAMO_NODE)
          continue;
        const auto *endpoint = endpoints->find(member.participant_id);
        if (ParticipantEndpointCache::is_online(endpoint, now_ms) &&
            endpoint->site_credentials_complete &&
            participant_endpoint_matches_site_authority(
                endpoint,
                handover_endpoint->site_configuration_authority_epoch,
                handover_endpoint->site_configuration_generation,
                handover_endpoint->site_configuration_digest,
                handover_endpoint->site_topology_revision,
                handover_endpoint->site_topology_digest))
          ++matching_replicas;
      }
      if (matching_replicas <
          site_authority_rejoin_quorum_required(configuration)) {
        const uint32_t slot_delay = site_replication_source_slot_delay_ms(
            configuration, handover_source, identity.node_id);
        if (slot_delay == std::numeric_limits<uint32_t>::max())
          return candidate;
        candidate.available = true;
        std::strncpy(candidate.source_node_id, handover_source,
                     sizeof(candidate.source_node_id) - 1);
        candidate.source_node_id[sizeof(candidate.source_node_id) - 1] = '\0';
        candidate.authority_epoch =
            handover_endpoint->site_configuration_authority_epoch;
        candidate.generation =
            handover_endpoint->site_configuration_generation;
        candidate.configuration_digest =
            handover_endpoint->site_configuration_digest;
        candidate.slot_delay_ms = slot_delay;
        candidate.mode =
            SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER;
        return candidate;
      }
    }

    source = nullptr;
    source_endpoint = nullptr;
    SiteAuthorityReplicationEvidence selected_evidence{};
    for (const auto &seed_member : configuration.members) {
      if (!seed_member.occupied ||
          seed_member.membership != ParticipantMembership::ACTIVE ||
          seed_member.type != ParticipantType::NAMO_NODE ||
          std::strcmp(seed_member.participant_id, identity.node_id) == 0)
        continue;
      const auto *seed = endpoints->find(seed_member.participant_id);
      if (!endpoint_can_restore_quorum_authority(seed) ||
          seed->site_configuration_digest == 0 ||
          seed->site_topology_revision == 0 ||
          seed->site_topology_digest == 0)
        continue;

      SiteAuthorityReplicationEvidence evidence{};
      evidence.authority_epoch =
          seed->site_configuration_authority_epoch;
      evidence.generation = seed->site_configuration_generation;
      evidence.configuration_digest = seed->site_configuration_digest;
      evidence.topology_revision = seed->site_topology_revision;
      evidence.topology_digest = seed->site_topology_digest;
      evidence.quorum_required =
          site_authority_rejoin_quorum_required(configuration);
      const char *evidence_source = nullptr;
      for (const auto &member : configuration.members) {
        if (!member.occupied ||
            member.membership != ParticipantMembership::ACTIVE ||
            member.type != ParticipantType::NAMO_NODE ||
            std::strcmp(member.participant_id, identity.node_id) == 0)
          continue;
        const auto *endpoint = endpoints->find(member.participant_id);
        if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
            !endpoint->site_credentials_complete ||
            endpoint->site_configuration_authority_epoch !=
                evidence.authority_epoch ||
            endpoint->site_configuration_generation != evidence.generation ||
            endpoint->site_configuration_digest !=
                evidence.configuration_digest ||
            endpoint->site_topology_revision != evidence.topology_revision ||
            endpoint->site_topology_digest != evidence.topology_digest)
          continue;
        ++evidence.endorsements;
        if (evidence_source == nullptr ||
            std::strcmp(member.participant_id, evidence_source) < 0)
          evidence_source = member.participant_id;
      }
      if (!evidence.confirmed() || evidence_source == nullptr)
        continue;
      const auto selected_order = compare_site_configuration_revision(
          evidence.authority_epoch, evidence.generation,
          selected_evidence.authority_epoch, selected_evidence.generation);
      if (selected_evidence.confirmed() &&
          selected_order == SiteConfigurationRevisionOrder::SAME &&
          evidence.configuration_digest !=
              selected_evidence.configuration_digest)
        return candidate;
      if (!selected_evidence.confirmed() ||
          selected_order == SiteConfigurationRevisionOrder::NEWER ||
          (selected_order == SiteConfigurationRevisionOrder::SAME &&
           evidence.configuration_digest ==
               selected_evidence.configuration_digest &&
           std::strcmp(evidence_source, source) < 0)) {
        selected_evidence = evidence;
        source = evidence_source;
      }
    }
    if (source == nullptr || !selected_evidence.confirmed())
      return candidate;
    source_endpoint = endpoints->find(source);
    candidate.evidence = selected_evidence;
    candidate.mode =
        SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN;
  } else {
    candidate.mode = SiteConfigurationReplicationMode::STRICT;
  }

  const uint32_t slot_delay = site_replication_source_slot_delay_ms(
      configuration, source, identity.node_id);
  if (slot_delay == std::numeric_limits<uint32_t>::max())
    return candidate;

  candidate.available = true;
  std::strncpy(candidate.source_node_id, source,
               sizeof(candidate.source_node_id) - 1);
  candidate.source_node_id[sizeof(candidate.source_node_id) - 1] = '\0';
  candidate.authority_epoch =
      source_endpoint->site_configuration_authority_epoch;
  candidate.generation = source_endpoint->site_configuration_generation;
  candidate.configuration_digest =
      source_endpoint->site_configuration_digest;
  candidate.slot_delay_ms = slot_delay;
  return candidate;
}

SiteAuthorityReplicationCandidate select_enrollment_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms) {
  SiteAuthorityReplicationCandidate candidate{};
  if (endpoints == nullptr)
    return candidate;
  const char *source = nullptr;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *participant = participants.record_at(slot);
    if (participant == nullptr ||
        participant->membership == ParticipantMembership::REVOKED)
      continue;
    if (source != nullptr)
      return candidate;
    source = participant->participant_id;
  }
  if (!authenticated_enrollment_source_allowed(
          identity, configuration, participants, credentials, source))
    return candidate;
  const auto *endpoint = endpoints->find(source);
  if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
      endpoint->site_configuration_authority_epoch == 0 ||
      endpoint->site_configuration_generation == 0 ||
      compare_site_configuration_revision(
          endpoint->site_configuration_authority_epoch,
          endpoint->site_configuration_generation,
          configuration.authority_epoch, configuration.generation) !=
          SiteConfigurationRevisionOrder::NEWER)
    return candidate;

  candidate.available = true;
  std::strncpy(candidate.source_node_id, source,
               sizeof(candidate.source_node_id) - 1);
  candidate.authority_epoch =
      endpoint->site_configuration_authority_epoch;
  candidate.generation = endpoint->site_configuration_generation;
  candidate.configuration_digest = endpoint->site_configuration_digest;
  candidate.mode =
      SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT;
  return candidate;
}

bool automatic_enrollment_floorplan_replication_allowed(
    const SiteAuthorityReplicationCandidate &candidate,
    const ParticipantEndpoint *source_endpoint,
    bool local_floorplan_stored, uint32_t local_config_hash,
    uint32_t local_image_hash) {
  return candidate.available &&
         candidate.mode ==
             SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT &&
         source_endpoint != nullptr &&
         std::strcmp(candidate.source_node_id,
                     source_endpoint->participant_id) == 0 &&
         source_endpoint->floorplan_replica_ready &&
         local_floorplan_stored && local_config_hash != 0 &&
         local_image_hash != 0 &&
         local_config_hash == source_endpoint->floorplan_config_hash &&
         local_image_hash == source_endpoint->floorplan_image_hash;
}

SiteAuthorityReplicationCandidate select_floorplan_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms) {
  SiteAuthorityReplicationCandidate candidate{};
  if (directory == nullptr || endpoints == nullptr ||
      directory->compatibility_mirror ||
      directory->legacy_single_floorplan_projection)
    return candidate;

  SiteReplicationRoute route{};
  if (resolve_site_replication_route(directory, configuration,
                                     identity.node_id, nullptr,
                                     &route) !=
      SiteReplicationRouteResult::OK)
    return candidate;
  const auto *endpoint = endpoints->find(route.floorplan.owner_node_id);
  if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
      endpoint->site_topology_revision != configuration.roster_revision ||
      endpoint->site_topology_digest != site_topology_digest(configuration) ||
      endpoint->site_configuration_generation !=
          route.floorplan.owner_generation ||
      endpoint->floorplan_geometry_version !=
          route.floorplan.geometry_version ||
      endpoint->floorplan_config_hash != route.floorplan.config_hash ||
      endpoint->floorplan_image_hash != route.floorplan.image_hash)
    return candidate;
  if (configuration.generation == route.floorplan.owner_generation &&
      site_replication_route_matches_configuration(route, configuration))
    return candidate;

  const uint32_t slot_delay = site_replication_source_slot_delay_ms(
      configuration, route.floorplan.owner_node_id, identity.node_id);
  if (slot_delay == std::numeric_limits<uint32_t>::max())
    return candidate;
  candidate.available = true;
  std::strncpy(candidate.source_node_id,
               route.floorplan.owner_node_id,
               sizeof(candidate.source_node_id) - 1);
  candidate.source_node_id[sizeof(candidate.source_node_id) - 1] = '\0';
  candidate.authority_epoch =
      endpoint->site_configuration_authority_epoch == 0
          ? 1
          : endpoint->site_configuration_authority_epoch;
  candidate.generation = route.floorplan.owner_generation;
  candidate.configuration_digest = endpoint->site_configuration_digest;
  candidate.slot_delay_ms = slot_delay;
  candidate.route = route;
  return candidate;
}

SiteAuthorityReplicationCandidate select_site_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms) {
  const auto authority = select_site_authority_replication_candidate(
      identity, configuration, endpoints, now_ms);
  if (authority.available)
    return authority;
  return select_floorplan_replication_candidate(
      identity, configuration, directory, endpoints, now_ms);
}

uint64_t SiteAuthorityReplicationTracker::candidate_fingerprint_(
    const SiteAuthorityReplicationCandidate &candidate) {
  uint64_t hash = 1469598103934665603ULL;
  const auto digest = [&hash](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
      hash ^= bytes[index];
      hash *= 1099511628211ULL;
    }
  };
  digest(candidate.source_node_id, std::strlen(candidate.source_node_id));
  digest(&candidate.configuration_digest,
         sizeof(candidate.configuration_digest));
  digest(&candidate.mode, sizeof(candidate.mode));
  digest(&candidate.route.manifest_bound,
         sizeof(candidate.route.manifest_bound));
  if (candidate.route.manifest_bound) {
    const auto &floorplan = candidate.route.floorplan;
    digest(floorplan.floorplan_id, std::strlen(floorplan.floorplan_id));
    digest(floorplan.owner_node_id, std::strlen(floorplan.owner_node_id));
    digest(&floorplan.owner_generation, sizeof(floorplan.owner_generation));
    digest(&floorplan.geometry_version, sizeof(floorplan.geometry_version));
    digest(&floorplan.config_hash, sizeof(floorplan.config_hash));
    digest(&floorplan.image_hash, sizeof(floorplan.image_hash));
  }
  return hash;
}

bool SiteAuthorityReplicationTracker::same_candidate_(
    const Revision &revision, uint32_t authority_epoch, uint32_t generation,
    uint64_t candidate_fingerprint) {
  return revision.authority_epoch == authority_epoch &&
         revision.generation == generation &&
         revision.candidate_fingerprint == candidate_fingerprint;
}

bool SiteAuthorityReplicationTracker::transport_available_(
    SiteReplicationState state) {
  return state == SiteReplicationState::IDLE ||
         state == SiteReplicationState::COMPLETE ||
         state == SiteReplicationState::FAILED ||
         state == SiteReplicationState::CONFLICT;
}

void SiteAuthorityReplicationTracker::reset() {
  *this = SiteAuthorityReplicationTracker{};
}

void SiteAuthorityReplicationTracker::observe_candidate(
    const SiteAuthorityReplicationCandidate &candidate, uint32_t now_ms) {
  const uint64_t fingerprint = candidate_fingerprint_(candidate);
  const bool same_active =
      same_candidate_(this->active_, candidate.authority_epoch,
                      candidate.generation, fingerprint);
  const bool same_pending =
      same_candidate_(this->pending_, candidate.authority_epoch,
                      candidate.generation, fingerprint);
  const bool same_blocked =
      same_candidate_(this->blocked_, candidate.authority_epoch,
                      candidate.generation, fingerprint);
  if (!candidate.available || candidate.source_node_id[0] == '\0' ||
      candidate.authority_epoch == 0 || candidate.generation == 0 ||
      same_active || same_pending || same_blocked)
    return;

  this->pending_ = {candidate.authority_epoch, candidate.generation,
                    fingerprint};
  this->due_ms_ = now_ms + candidate.slot_delay_ms;
  this->retry_count_ = 0;
  this->blocked_ = {};
  std::strncpy(this->source_node_id_, candidate.source_node_id,
               sizeof(this->source_node_id_) - 1);
  this->source_node_id_[sizeof(this->source_node_id_) - 1] = '\0';
  this->replication_mode_ = candidate.mode;
  this->route_ = candidate.route;
}

void SiteAuthorityReplicationTracker::schedule_retry_(
    const Revision &revision, uint32_t now_ms) {
  if (this->retry_count_ < SITE_REPLICATION_MAX_SESSION_RETRIES) {
    ++this->retry_count_;
    this->pending_ = revision;
    this->due_ms_ =
        now_ms + site_replication_retry_delay_ms(this->retry_count_);
    return;
  }
  this->retry_count_ = 0;
  this->pending_ = revision;
  this->due_ms_ = now_ms + FAILURE_COOLDOWN_MS;
}

void SiteAuthorityReplicationTracker::observe_transport(
    const SiteConfigurationSnapshot &local,
    SiteReplicationState transport_state, uint32_t now_ms) {
  if (this->active_.generation == 0)
    return;
  const bool route_complete =
      this->route_.manifest_bound &&
      local.generation == this->route_.floorplan.owner_generation &&
      site_replication_route_matches_configuration(this->route_, local);
  const bool legacy_complete =
      !this->route_.manifest_bound &&
      compare_site_configuration_revision(
          local.authority_epoch, local.generation,
          this->active_.authority_epoch, this->active_.generation) !=
          SiteConfigurationRevisionOrder::OLDER;
  if (route_complete || legacy_complete ||
      transport_state == SiteReplicationState::COMPLETE) {
    this->active_ = {};
    this->retry_count_ = 0;
    this->route_ = {};
    return;
  }
  if (transport_state == SiteReplicationState::FAILED) {
    const Revision failed = this->active_;
    this->active_ = {};
    if (compare_site_configuration_revision(
            this->pending_.authority_epoch, this->pending_.generation,
            failed.authority_epoch, failed.generation) ==
        SiteConfigurationRevisionOrder::NEWER) {
      this->retry_count_ = 0;
      return;
    }
    this->schedule_retry_(failed, now_ms);
    return;
  }
  if (transport_state == SiteReplicationState::CONFLICT) {
    this->blocked_ = this->active_;
    this->active_ = {};
    this->pending_ = {};
    this->due_ms_ = 0;
    this->route_ = {};
  }
}

bool SiteAuthorityReplicationTracker::start_due(
    uint32_t now_ms, SiteReplicationState transport_state) const {
  return this->pending_.generation != 0 &&
         static_cast<int32_t>(now_ms - this->due_ms_) >= 0 &&
         transport_available_(transport_state);
}

void SiteAuthorityReplicationTracker::complete_start(
    SiteReplicationResult result, uint32_t now_ms) {
  if (this->pending_.generation == 0)
    return;
  if (result == SiteReplicationResult::OK) {
    this->active_ = this->pending_;
    this->pending_ = {};
    this->due_ms_ = 0;
    return;
  }
  this->schedule_retry_(this->pending_, now_ms);
}

uint32_t SiteAuthorityReplicationTracker::wait_remaining_ms(
    uint32_t now_ms) const {
  if (this->pending_.generation == 0 ||
      static_cast<int32_t>(now_ms - this->due_ms_) >= 0)
    return 0;
  return this->due_ms_ - now_ms;
}

}  // namespace radar_api_server
}  // namespace esphome
