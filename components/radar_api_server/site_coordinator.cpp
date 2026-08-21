#include "site_coordinator.h"

#include "site_directory.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

void SiteCoordinator::set_local_directory_fence(uint32_t revision,
                                                 uint32_t digest) {
  this->local_directory_revision_ = revision;
  this->local_directory_digest_ = digest;
}

void SiteCoordinator::update(const PlatformIdentity &identity,
                             const ParticipantRegistry &participants,
                             uint32_t now_ms) {
  this->update(identity, participants, nullptr, now_ms);
}

void SiteCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration, uint32_t now_ms) {
  this->update(identity, participants, configuration, true, now_ms);
}

void SiteCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    bool local_credentials_complete, uint32_t now_ms) {
  this->update(identity, participants, configuration, nullptr,
               local_credentials_complete, now_ms);
}

void SiteCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, bool local_credentials_complete,
    uint32_t now_ms) {
  if (!valid_text_(identity.node_id, PlatformIdentity::MAX_ID_LENGTH))
    return;

  char leader_before[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  copy_(leader_before, sizeof(leader_before), this->state_.leader_node_id);
  const uint32_t previous_term = this->state_.term;
  const uint8_t previous_expected = this->state_.participants_expected;
  const uint8_t previous_online = this->state_.participants_online;
  const bool previous_local_leader = this->state_.local_leader;
  const bool configuration_ready =
      configuration != nullptr && configuration->roster_committed &&
      std::strcmp(configuration->site_id, identity.site_id) == 0 &&
      site_configuration_member_active(*configuration, identity.node_id);
  const bool canonical_multi_node =
      configuration_ready && configuration->active_member_count() > 1;
  if (this->standalone_authority_held_ && canonical_multi_node)
    this->standalone_handoff_pending_ = true;
  this->state_.configuration_ready = configuration_ready;
  this->state_.roster_revision =
      configuration_ready ? configuration->roster_revision : 0;
  this->state_.participants_expected = static_cast<uint8_t>(
      configuration_ready ? configuration->active_member_count()
                          : 1U + participants.active_count());
  this->local_credentials_complete_ = local_credentials_complete;
  this->reconcile_heartbeat_slots_(
      participants, configuration_ready ? configuration : nullptr, now_ms);
  this->elect_leader_(identity, participants,
                      configuration_ready ? configuration : nullptr,
                      configuration_ready ? directory : nullptr, now_ms);
  this->election_roster_revision_ =
      configuration_ready ? configuration->roster_revision : 0;

  if (!canonical_multi_node && this->state_.participants_expected == 1 &&
      this->state_.local_leader && this->state_.term != 0 &&
      std::strcmp(this->state_.leader_node_id, identity.node_id) == 0) {
    this->standalone_authority_held_ = true;
    this->standalone_authority_term_ = this->state_.term;
    this->standalone_handoff_pending_ = false;
  } else if (canonical_multi_node && this->state_.quorum_available &&
             this->state_.leader_node_id[0] != '\0') {
    this->standalone_authority_held_ = false;
    this->standalone_authority_term_ = 0;
    this->standalone_handoff_pending_ = false;
  }

  if (previous_term != this->state_.term ||
      previous_expected != this->state_.participants_expected ||
      previous_online != this->state_.participants_online ||
      previous_local_leader != this->state_.local_leader ||
      std::strcmp(leader_before, this->state_.leader_node_id) != 0 ||
      this->generation_ == 0)
    this->generation_++;
}

SiteHeartbeatAdmissionResult SiteCoordinator::accept_heartbeat(
    const ParticipantRegistry &participants,
    const SiteLeaderHeartbeat &heartbeat, uint32_t now_ms) {
  return this->accept_heartbeat(participants, nullptr, heartbeat, now_ms);
}

SiteHeartbeatAdmissionResult SiteCoordinator::accept_heartbeat(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    const SiteLeaderHeartbeat &heartbeat, uint32_t now_ms) {
  if (!valid_text_(heartbeat.participant_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(heartbeat.boot_id,
                   SiteLeaderHeartbeat::MAX_BOOT_ID_LENGTH) ||
      heartbeat.sequence == 0 || heartbeat.term == 0 ||
      heartbeat.lease_ms < MIN_LEASE_MS ||
      heartbeat.lease_ms > MAX_LEASE_MS)
    return SiteHeartbeatAdmissionResult::INVALID_ARGUMENT;
  if (heartbeat.sample_age_ms >= heartbeat.lease_ms)
    return SiteHeartbeatAdmissionResult::EXPIRED;

  const bool configuration_ready =
      configuration != nullptr && configuration->roster_committed;
  const auto *participant = participants.find(heartbeat.participant_id);
  if (participant != nullptr &&
      participant->membership == ParticipantMembership::REVOKED)
    return SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE;
  if (configuration_ready) {
    if (!site_configuration_member_active(*configuration,
                                          heartbeat.participant_id))
      return SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE;
  } else {
    if (participant == nullptr)
      return SiteHeartbeatAdmissionResult::UNKNOWN_PARTICIPANT;
    if (participant->membership != ParticipantMembership::ACTIVE)
      return SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE;
  }
  const bool leader_eligible =
      configuration_ready
          ? site_configuration_member_active(
                *configuration, heartbeat.participant_id, true)
          : participant->type == ParticipantType::NAMO_NODE &&
                participant->leader_eligible;
  HeartbeatSlot *source = this->ensure_heartbeat_(heartbeat.participant_id);
  if (source == nullptr)
    return SiteHeartbeatAdmissionResult::FULL;
  const bool same_boot = source->occupied &&
                         std::strcmp(source->boot_id, heartbeat.boot_id) == 0;
  if (same_boot && heartbeat.sequence <= source->sequence)
    return SiteHeartbeatAdmissionResult::DUPLICATE_OR_REORDERED;

  const bool authority_lease_active =
      this->state_.leader_node_id[0] != '\0' &&
      !time_reached_(now_ms, this->state_.lease_expires_at_ms);
  const bool sender_is_current_leader =
      this->state_.leader_node_id[0] != '\0' &&
      std::strcmp(this->state_.leader_node_id,
                  heartbeat.participant_id) == 0;
  const bool stale_active_leader_term =
      leader_eligible && sender_is_current_leader &&
      heartbeat.term < this->state_.term && authority_lease_active;
  const bool stale_term = stale_active_leader_term;

  source->occupied = true;
  copy_(source->participant_id, sizeof(source->participant_id),
        heartbeat.participant_id);
  copy_(source->boot_id, sizeof(source->boot_id), heartbeat.boot_id);
  source->sequence = heartbeat.sequence;
  source->term = heartbeat.term;
  source->term_admitted = !stale_term;
  source->roster_revision = heartbeat.roster_revision;
  source->election_view_digest = heartbeat.election_view_digest;
  source->election_contract_version = heartbeat.election_contract_version;
  source->credentials_complete = heartbeat.credentials_complete;
  source->expires_at_ms =
      now_ms + (heartbeat.lease_ms - heartbeat.sample_age_ms);
  if (heartbeat.directory_revision != 0 && heartbeat.directory_digest != 0) {
    source->directory_revision = heartbeat.directory_revision;
    source->directory_digest = heartbeat.directory_digest;
  }
  if (stale_term)
    return SiteHeartbeatAdmissionResult::STALE_TERM;

  return SiteHeartbeatAdmissionResult::OK;
}

void SiteCoordinator::clear() {
  this->state_ = {};
  this->heartbeats_ = {};
  this->generation_ = 0;
  this->suspended_leader_node_id_[0] = '\0';
  this->suspended_leader_term_ = 0;
  this->local_directory_revision_ = 0;
  this->local_directory_digest_ = 0;
  this->initial_probe_started_ms_ = 0;
  this->standalone_authority_term_ = 0;
  this->initial_probe_active_ = false;
  this->initial_probe_complete_ = false;
  this->local_credentials_complete_ = true;
  this->standalone_authority_held_ = false;
  this->standalone_handoff_pending_ = false;
  this->election_roster_revision_ = 0;
  this->transition_views_.reset();
}

uint32_t SiteCoordinator::lease_remaining_ms(
    const SiteCoordinatorState &state, uint32_t now_ms) {
  if (time_reached_(now_ms, state.lease_expires_at_ms))
    return 0;
  return state.lease_expires_at_ms - now_ms;
}

SiteCoordinator::HeartbeatSlot *SiteCoordinator::find_heartbeat_(
    const char *participant_id) {
  for (auto &heartbeat : this->heartbeats_) {
    if (heartbeat.occupied &&
        std::strcmp(heartbeat.participant_id, participant_id) == 0)
      return &heartbeat;
  }
  return nullptr;
}

SiteCoordinator::HeartbeatSlot *SiteCoordinator::ensure_heartbeat_(
    const char *participant_id) {
  HeartbeatSlot *existing = this->find_heartbeat_(participant_id);
  if (existing != nullptr)
    return existing;
  for (auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied)
      return &heartbeat;
  }
  return nullptr;
}

void SiteCoordinator::reconcile_heartbeat_slots_(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration, uint32_t now_ms) {
  for (auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied)
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    bool active = participant == nullptr ||
                  participant->membership != ParticipantMembership::REVOKED;
    if (active) {
      active = configuration != nullptr
                   ? site_configuration_member_active(
                         *configuration, heartbeat.participant_id)
                   : participant != nullptr &&
                         participant->membership ==
                             ParticipantMembership::ACTIVE;
    }
    if (!active || time_reached_(now_ms, heartbeat.expires_at_ms))
      heartbeat = HeartbeatSlot{};
  }
}

void SiteCoordinator::elect_leader_(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    const SiteDirectorySnapshot *directory, uint32_t now_ms) {
  const bool prior_quorum_available = this->state_.quorum_available;
  const uint32_t prior_term = this->state_.term;
  const uint32_t prior_view_digest = this->state_.election_view_digest;
  const uint32_t prior_compatibility_v11_view_digest =
      this->state_.compatibility_v11_election_view_digest;
  const uint32_t prior_legacy_view_digest =
      this->state_.previous_election_view_digest;
  const bool local_active =
      configuration == nullptr ||
      site_configuration_member_active(*configuration, identity.node_id);
  const bool local_eligible =
      local_active && this->local_credentials_complete_ &&
      (configuration == nullptr ||
                       site_configuration_member_active(
                           *configuration, identity.node_id, true));
  bool eligible_remote_observed = false;
  std::size_t eligible_member_count = local_eligible ? 1U : 0U;
  if (configuration != nullptr) {
    for (const auto &member : configuration->members) {
      if (member.occupied &&
          std::strcmp(member.participant_id, identity.node_id) != 0 &&
          site_configuration_member_active(*configuration,
                                           member.participant_id, true))
        ++eligible_member_count;
    }
  }
  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied || time_reached_(now_ms, heartbeat.expires_at_ms))
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    if (participant != nullptr &&
        participant->membership == ParticipantMembership::REVOKED)
      continue;
    if (configuration != nullptr) {
      if (!site_configuration_member_active(*configuration,
                                            heartbeat.participant_id))
        continue;
    } else if (participant == nullptr ||
               participant->membership != ParticipantMembership::ACTIVE) {
      continue;
    }
    const bool eligible =
        configuration != nullptr
            ? site_configuration_member_active(
                  *configuration, heartbeat.participant_id, true)
            : participant->type == ParticipantType::NAMO_NODE &&
                  participant->leader_eligible;
    if (eligible && heartbeat.credentials_complete) {
      eligible_remote_observed = true;
      break;
    }
  }
  const bool multi_node_probe_required =
      configuration != nullptr && eligible_member_count > 1 &&
      local_eligible && !this->initial_probe_complete_;
  if (multi_node_probe_required && !eligible_remote_observed) {
    if (!this->initial_probe_active_) {
      this->initial_probe_active_ = true;
      this->initial_probe_started_ms_ = now_ms;
    }
    if (now_ms - this->initial_probe_started_ms_ < DEFAULT_LEASE_MS) {
      RoleElectionView probe_view{};
      if (local_active) {
        probe_view.add(
            identity.node_id,
            (static_cast<uint64_t>(this->local_directory_revision_) << 32U) |
                this->local_directory_digest_,
            this->local_credentials_complete_);
      }
      this->state_.leader_node_id[0] = '\0';
      this->state_.local_leader = false;
      this->state_.quorum_required =
          strict_majority(this->state_.participants_expected);
      this->state_.quorum_available = false;
      this->state_.term = this->state_.term == 0 ? 1 : this->state_.term;
      this->state_.lease_expires_at_ms =
          this->initial_probe_started_ms_ + DEFAULT_LEASE_MS;
      this->state_.participants_online = 1;
      this->state_.previous_election_view_digest = probe_view.digest("site");
      this->state_.election_view_digest =
          probe_view.winner_contract_digest("site", "");
      this->state_.compatibility_v11_election_view_digest =
          this->state_.election_view_digest;
      this->state_.election_endorsements = local_active ? 1 : 0;
      this->state_.election_fenced = false;
      return;
    }
    this->initial_probe_complete_ = true;
    this->initial_probe_active_ = false;
  } else if (eligible_remote_observed) {
    this->initial_probe_complete_ = true;
    this->initial_probe_active_ = false;
  } else if (!multi_node_probe_required && configuration != nullptr) {
    this->initial_probe_complete_ = true;
    this->initial_probe_active_ = false;
  }
  const uint8_t directory_quorum_required =
      strict_majority(this->state_.participants_expected);
  const bool current_leader_lease_active =
      this->state_.leader_node_id[0] != '\0' &&
      !time_reached_(now_ms, this->state_.lease_expires_at_ms);
  bool directory_fence_observed =
      this->local_directory_revision_ != 0 &&
      this->local_directory_digest_ != 0;
  for (const auto &heartbeat : this->heartbeats_) {
    if (heartbeat.occupied &&
        !time_reached_(now_ms, heartbeat.expires_at_ms) &&
        heartbeat.directory_revision != 0 &&
        heartbeat.directory_digest != 0) {
      directory_fence_observed = true;
      break;
    }
  }
  const bool directory_quorum_required_for_leader =
      configuration != nullptr && this->state_.participants_expected > 1 &&
      directory_fence_observed;
  const auto exact_directory_support =
      [&](uint32_t revision, uint32_t digest) -> uint8_t {
    if (revision == 0 || digest == 0)
      return 0;
    uint8_t support =
        local_active && this->local_credentials_complete_ &&
                this->local_directory_revision_ == revision &&
                this->local_directory_digest_ == digest
            ? 1
            : 0;
    for (const auto &heartbeat : this->heartbeats_) {
      if (!heartbeat.occupied ||
          time_reached_(now_ms, heartbeat.expires_at_ms) ||
          !heartbeat.credentials_complete ||
          heartbeat.directory_revision != revision ||
          heartbeat.directory_digest != digest)
        continue;
      if (configuration != nullptr) {
        if (site_configuration_member_active(*configuration,
                                             heartbeat.participant_id))
          ++support;
      } else {
        const auto *participant = participants.find(heartbeat.participant_id);
        if (participant != nullptr &&
            participant->membership == ParticipantMembership::ACTIVE)
          ++support;
      }
    }
    return support;
  };
  uint8_t directory_observers =
      local_active && this->local_credentials_complete_ &&
              this->local_directory_revision_ != 0 &&
              this->local_directory_digest_ != 0
          ? 1
          : 0;
  bool exact_directory_quorum_exists =
      exact_directory_support(this->local_directory_revision_,
                              this->local_directory_digest_) >=
      directory_quorum_required;
  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied ||
        time_reached_(now_ms, heartbeat.expires_at_ms) ||
        !heartbeat.credentials_complete ||
        heartbeat.directory_revision == 0 || heartbeat.directory_digest == 0)
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    const bool active =
        configuration != nullptr
            ? site_configuration_member_active(*configuration,
                                               heartbeat.participant_id)
            : participant != nullptr &&
                  participant->membership == ParticipantMembership::ACTIVE;
    if (!active)
      continue;
    ++directory_observers;
    if (exact_directory_support(heartbeat.directory_revision,
                                heartbeat.directory_digest) >=
        directory_quorum_required)
      exact_directory_quorum_exists = true;
  }
  const auto directory_candidate_allowed =
      [&](const char *candidate_id, uint32_t revision, uint32_t digest) {
    if (!directory_quorum_required_for_leader)
      return true;
    if (exact_directory_support(revision, digest) >=
        directory_quorum_required)
      return true;
    if (current_leader_lease_active && candidate_id != nullptr &&
        std::strcmp(candidate_id, this->state_.leader_node_id) == 0)
      return true;
    // No fork has majority evidence; choose one deterministically so Directory
    // replication can resume instead of leaving every survivor leaderless.
    return !exact_directory_quorum_exists &&
           directory_observers >= directory_quorum_required;
  };

  const bool local_candidate_allowed =
      local_eligible && directory_candidate_allowed(
                            identity.node_id, this->local_directory_revision_,
                            this->local_directory_digest_);
  const char *winner = local_candidate_allowed ? identity.node_id : nullptr;
  uint8_t winner_penalty =
      local_candidate_allowed
          ? site_leader_role_penalty(configuration, directory,
                                     identity.node_id)
          : 0;
  const HeartbeatSlot *winner_heartbeat = nullptr;
  uint32_t winner_directory_revision =
      local_candidate_allowed ? this->local_directory_revision_ : 0;
  uint32_t winner_directory_digest =
      local_candidate_allowed ? this->local_directory_digest_ : 0;
  bool incumbent_candidate_ready =
      configuration != nullptr && current_leader_lease_active &&
      local_candidate_allowed &&
      std::strcmp(this->state_.leader_node_id, identity.node_id) == 0 &&
      (!exact_directory_quorum_exists ||
       exact_directory_support(this->local_directory_revision_,
                               this->local_directory_digest_) >=
           directory_quorum_required);
  const HeartbeatSlot *incumbent_heartbeat = nullptr;
  uint8_t incumbent_penalty = winner_penalty;
  uint32_t incumbent_directory_revision = this->local_directory_revision_;
  uint32_t incumbent_directory_digest = this->local_directory_digest_;
  uint8_t online = local_active ? 1 : 0;
  uint32_t observed_term = this->state_.term;

  const auto candidate_preferred = [](const char *candidate_id,
                                      uint8_t candidate_penalty,
                                      uint32_t candidate_revision,
                                      uint32_t candidate_digest,
                                      const char *current_id,
                                      uint8_t current_penalty,
                                      uint32_t current_revision,
                                      uint32_t current_digest) {
    const bool candidate_fenced =
        candidate_revision != 0 && candidate_digest != 0;
    const bool current_fenced = current_revision != 0 && current_digest != 0;
    if (candidate_fenced != current_fenced)
      return candidate_fenced;
    if (candidate_fenced && candidate_revision != current_revision)
      return candidate_revision > current_revision;
    return leader_candidate_preferred(candidate_id, candidate_penalty,
                                      current_id, current_penalty);
  };

  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied || time_reached_(now_ms, heartbeat.expires_at_ms))
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    if (participant != nullptr &&
        participant->membership == ParticipantMembership::REVOKED)
      continue;
    if (configuration != nullptr) {
      if (!site_configuration_member_active(*configuration,
                                            heartbeat.participant_id))
        continue;
    } else if (participant == nullptr ||
               participant->membership != ParticipantMembership::ACTIVE) {
      continue;
    }
    online++;
    const bool leader_eligible =
        configuration != nullptr
            ? site_configuration_member_active(
                  *configuration, heartbeat.participant_id, true)
            : participant->type == ParticipantType::NAMO_NODE &&
                  participant->leader_eligible;
    if (!leader_eligible || !heartbeat.credentials_complete ||
        !directory_candidate_allowed(
            heartbeat.participant_id, heartbeat.directory_revision,
            heartbeat.directory_digest))
      continue;
    if (heartbeat.term_admitted && heartbeat.term > observed_term)
      observed_term = heartbeat.term;
    const uint8_t candidate_penalty =
        site_leader_role_penalty(configuration, directory,
                                 heartbeat.participant_id);
    if (configuration != nullptr && current_leader_lease_active &&
        std::strcmp(this->state_.leader_node_id,
                    heartbeat.participant_id) == 0 &&
        (!exact_directory_quorum_exists ||
         exact_directory_support(heartbeat.directory_revision,
                                 heartbeat.directory_digest) >=
             directory_quorum_required)) {
      incumbent_candidate_ready = true;
      incumbent_heartbeat = &heartbeat;
      incumbent_penalty = candidate_penalty;
      incumbent_directory_revision = heartbeat.directory_revision;
      incumbent_directory_digest = heartbeat.directory_digest;
    }
    if (candidate_preferred(
            heartbeat.participant_id, candidate_penalty,
            heartbeat.directory_revision, heartbeat.directory_digest, winner,
            winner_penalty, winner_directory_revision,
            winner_directory_digest)) {
      winner = heartbeat.participant_id;
      winner_penalty = candidate_penalty;
      winner_heartbeat = &heartbeat;
      winner_directory_revision = heartbeat.directory_revision;
      winner_directory_digest = heartbeat.directory_digest;
    }
  }
  const bool incumbent_selected = incumbent_candidate_ready;
  if (incumbent_selected) {
    winner = this->state_.leader_node_id;
    winner_penalty = incumbent_penalty;
    winner_heartbeat = incumbent_heartbeat;
    winner_directory_revision = incumbent_directory_revision;
    winner_directory_digest = incumbent_directory_digest;
  }

  const uint64_t local_directory_qualifier =
      (static_cast<uint64_t>(this->local_directory_revision_) << 32U) |
      this->local_directory_digest_;
  bool remote_fence_observed = false;
  uint32_t previous_view_digest = 0;
  uint32_t observed_winner_digest = 0;
  {
    SiteRoleElectionView observed_election_view{};
    if (local_active)
      observed_election_view.add(identity.node_id, local_directory_qualifier,
                                 this->local_credentials_complete_);
    for (const auto &heartbeat : this->heartbeats_) {
      if (!heartbeat.occupied ||
          time_reached_(now_ms, heartbeat.expires_at_ms))
        continue;
      if (configuration != nullptr) {
        if (!site_configuration_member_active(*configuration,
                                              heartbeat.participant_id))
          continue;
      } else {
        const auto *participant = participants.find(heartbeat.participant_id);
        if (participant == nullptr ||
            participant->membership != ParticipantMembership::ACTIVE)
          continue;
      }
      observed_election_view.add(
          heartbeat.participant_id,
          (static_cast<uint64_t>(heartbeat.directory_revision) << 32U) |
              heartbeat.directory_digest,
          heartbeat.credentials_complete);
      if (heartbeat.election_view_digest != 0)
        remote_fence_observed = true;
    }
    previous_view_digest = observed_election_view.digest("site");
    observed_winner_digest =
        observed_election_view.winner_contract_digest("site", winner);
  }
  uint32_t view_digest = observed_winner_digest;
  uint32_t compatibility_v11_view_digest = observed_winner_digest;
  if (configuration != nullptr) {
    // The election contract is the committed Site roster, not the subset of
    // heartbeats that happened to arrive at this Node. Liveness is counted
    // separately below. This keeps a returning member from changing the
    // contract independently on each receiver.
    const auto contract_digest_for = [&](uint8_t contract_version,
                                         const char *selected_winner) {
      SiteRoleElectionView election_view{};
      if (contract_version >= 3) {
        const uint64_t roster_qualifier = configuration->roster_revision;
        for (const auto &member : configuration->members) {
          if (!member.occupied ||
              member.membership != ParticipantMembership::ACTIVE)
            continue;
          election_view.add(member.participant_id, roster_qualifier, true);
        }
      } else {
        if (local_active)
          election_view.add(identity.node_id, local_directory_qualifier,
                            this->local_credentials_complete_);
        for (const auto &heartbeat : this->heartbeats_) {
          if (!heartbeat.occupied ||
              time_reached_(now_ms, heartbeat.expires_at_ms) ||
              !site_configuration_member_active(
                  *configuration, heartbeat.participant_id))
            continue;
          election_view.add(
              heartbeat.participant_id,
              (static_cast<uint64_t>(heartbeat.directory_revision) << 32U) |
                  heartbeat.directory_digest,
              heartbeat.credentials_complete);
        }
      }
      return election_view.winner_contract_digest("site", selected_winner);
    };

    // A freshly booted Node has no incumbent memory. Recover the established
    // winner from a modern digest already endorsed by a remote strict
    // majority. The canonical roster lets us test every bounded eligible
    // candidate without adding another identity field to the heartbeat wire.
    if (!incumbent_selected &&
        this->state_.participants_expected > 1) {
      const uint8_t required =
          strict_majority(this->state_.participants_expected);
      uint32_t endorsed_digest = 0;
      uint8_t endorsed_contract_version = 0;
      for (const uint8_t contract_version : {3U, 2U}) {
        for (const auto &candidate_vote : this->heartbeats_) {
          if (!candidate_vote.occupied ||
              time_reached_(now_ms, candidate_vote.expires_at_ms) ||
              candidate_vote.election_contract_version != contract_version ||
              candidate_vote.election_view_digest == 0 ||
              !candidate_vote.credentials_complete ||
              !site_configuration_member_active(
                  *configuration, candidate_vote.participant_id))
            continue;
          uint8_t support = 0;
          for (const auto &vote : this->heartbeats_) {
            if (vote.occupied &&
                !time_reached_(now_ms, vote.expires_at_ms) &&
                vote.election_contract_version == contract_version &&
                vote.credentials_complete &&
                vote.election_view_digest ==
                    candidate_vote.election_view_digest &&
                site_configuration_member_active(*configuration,
                                                  vote.participant_id))
              ++support;
          }
          if (support >= required) {
            endorsed_digest = candidate_vote.election_view_digest;
            endorsed_contract_version = contract_version;
            break;
          }
        }
        if (endorsed_digest != 0)
          break;
      }

      if (endorsed_digest != 0) {
        for (const auto &member : configuration->members) {
          if (!member.occupied ||
              member.membership != ParticipantMembership::ACTIVE ||
              member.type != ParticipantType::NAMO_NODE ||
              !member.leader_eligible ||
              contract_digest_for(endorsed_contract_version,
                                  member.participant_id) != endorsed_digest)
            continue;

          if (std::strcmp(member.participant_id, identity.node_id) == 0) {
            if (!local_candidate_allowed)
              continue;
            winner = identity.node_id;
            winner_penalty =
                site_leader_role_penalty(configuration, directory, winner);
            winner_heartbeat = nullptr;
            winner_directory_revision = this->local_directory_revision_;
            winner_directory_digest = this->local_directory_digest_;
            break;
          }

          const HeartbeatSlot *endorsed_winner = nullptr;
          for (const auto &heartbeat : this->heartbeats_) {
            if (heartbeat.occupied &&
                !time_reached_(now_ms, heartbeat.expires_at_ms) &&
                heartbeat.credentials_complete &&
                std::strcmp(heartbeat.participant_id,
                            member.participant_id) == 0) {
              endorsed_winner = &heartbeat;
              break;
            }
          }
          if (endorsed_winner == nullptr ||
              !directory_candidate_allowed(
                  endorsed_winner->participant_id,
                  endorsed_winner->directory_revision,
                  endorsed_winner->directory_digest))
            continue;
          winner = endorsed_winner->participant_id;
          winner_penalty =
              site_leader_role_penalty(configuration, directory, winner);
          winner_heartbeat = endorsed_winner;
          winner_directory_revision = endorsed_winner->directory_revision;
          winner_directory_digest = endorsed_winner->directory_digest;
          break;
        }
      }
    }
    view_digest = contract_digest_for(3, winner);
    compatibility_v11_view_digest = contract_digest_for(2, winner);
  }
  const bool incumbent_transition_allowed =
      prior_quorum_available && prior_term != 0 && incumbent_selected &&
      role_id_equal(this->state_.leader_node_id, winner) &&
      observed_term <= prior_term;
  const bool local_roster_transition =
      configuration != nullptr && this->election_roster_revision_ != 0 &&
      configuration->roster_revision != this->election_roster_revision_;
  const bool remote_incumbent_roster_transition =
      incumbent_heartbeat != nullptr &&
      incumbent_heartbeat->roster_revision != 0 && configuration != nullptr &&
      incumbent_heartbeat->roster_revision != configuration->roster_revision;
  bool remote_roster_transition_observed = false;
  if (configuration != nullptr) {
    for (const auto &heartbeat : this->heartbeats_) {
      if (heartbeat.occupied &&
          !time_reached_(now_ms, heartbeat.expires_at_ms) &&
          heartbeat.credentials_complete && heartbeat.roster_revision != 0 &&
          heartbeat.roster_revision != configuration->roster_revision) {
        remote_roster_transition_observed = true;
        break;
      }
    }
  }
  const bool bounded_incumbent_transition =
      incumbent_transition_allowed &&
      (local_roster_transition || remote_incumbent_roster_transition ||
       remote_roster_transition_observed || this->transition_views_.active());
  const bool election_contract_changed =
      view_digest != prior_view_digest ||
      compatibility_v11_view_digest !=
          prior_compatibility_v11_view_digest ||
      previous_view_digest != prior_legacy_view_digest;
  if (!bounded_incumbent_transition) {
    this->transition_views_.reset();
  } else if (election_contract_changed) {
    this->transition_views_.capture(
        prior_view_digest, prior_compatibility_v11_view_digest,
        prior_legacy_view_digest, prior_legacy_view_digest);
  }
  uint8_t endorsements = local_active ? 1 : 0;
  uint8_t transition_view_endorsements = 0;
  BoundedRoleElectionDigestVotes<
      ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      alternate_revision_votes{};
  const bool incumbent_remote_transition_available =
      bounded_incumbent_transition && remote_incumbent_roster_transition &&
      incumbent_heartbeat != nullptr &&
      incumbent_heartbeat->term_admitted &&
      incumbent_heartbeat->term <= prior_term &&
      incumbent_heartbeat->election_view_digest != 0;
  const bool alternate_revision_transition_available =
      bounded_incumbent_transition &&
      !incumbent_remote_transition_available;
  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied || time_reached_(now_ms, heartbeat.expires_at_ms))
      continue;
    const uint32_t expected_digest =
        heartbeat.election_contract_version >= 3
            ? view_digest
            : heartbeat.election_contract_version == 2
                  ? compatibility_v11_view_digest
                  : previous_view_digest;
    const bool endorses_current =
        heartbeat.election_view_digest == expected_digest;
    const bool endorses_transition =
        !endorses_current && this->transition_views_.matches(
                                  heartbeat.election_contract_version,
                                  heartbeat.election_view_digest);
    const bool endorses_incumbent_transition =
        !endorses_current && !endorses_transition &&
        incumbent_remote_transition_available &&
        heartbeat.election_contract_version ==
            incumbent_heartbeat->election_contract_version &&
        heartbeat.election_view_digest ==
            incumbent_heartbeat->election_view_digest;
    if (configuration != nullptr) {
      if (!site_configuration_member_active(*configuration,
                                            heartbeat.participant_id))
        continue;
    } else {
      const auto *participant = participants.find(heartbeat.participant_id);
      if (participant == nullptr ||
          participant->membership != ParticipantMembership::ACTIVE)
        continue;
    }
    if (!endorses_current && !endorses_transition &&
        !endorses_incumbent_transition) {
      if (alternate_revision_transition_available &&
          heartbeat.credentials_complete)
        alternate_revision_votes.add(
            heartbeat.election_contract_version,
            heartbeat.election_view_digest);
      continue;
    }
    if (endorses_current)
      ++endorsements;
    else
      ++transition_view_endorsements;
  }

  const auto quorum = evaluate_role_quorum(
      this->state_.participants_expected, online);
  const uint8_t direct_endorsements = endorsements;
  const bool fence_supported =
      configuration != nullptr && this->state_.participants_expected > 1 &&
      (directory_fence_observed || remote_fence_observed);
  const uint8_t alternate_transition_endorsements =
      alternate_revision_votes.strongest();
  if (alternate_transition_endorsements > transition_view_endorsements)
    transition_view_endorsements = alternate_transition_endorsements;
  if (endorsements >= quorum.required)
    this->transition_views_.reset();
  if (fence_supported)
    endorsements = preserve_incumbent_transition_endorsements(
        prior_quorum_available, this->state_.leader_node_id, prior_term,
        observed_term, winner, endorsements, transition_view_endorsements,
        quorum.required);
  const bool fence_available =
      !fence_supported || endorsements >= quorum.required;
  this->state_.quorum_required = quorum.required;
  this->state_.quorum_available = quorum.available && fence_available;
  this->state_.election_view_digest = view_digest;
  this->state_.compatibility_v11_election_view_digest =
      compatibility_v11_view_digest;
  this->state_.previous_election_view_digest = previous_view_digest;
  this->state_.election_endorsements = endorsements;
  this->state_.election_fenced = fence_supported;
  if (configuration != nullptr &&
      this->state_.participants_expected > 1 &&
      !this->state_.quorum_available) {
    if (this->state_.term == 0)
      this->state_.term = 1;
    if (this->state_.leader_node_id[0] != '\0') {
      copy_(this->suspended_leader_node_id_,
            sizeof(this->suspended_leader_node_id_),
            this->state_.leader_node_id);
      this->suspended_leader_term_ = this->state_.term;
    }
    this->state_.leader_node_id[0] = '\0';
    this->state_.local_leader = false;
    this->state_.lease_expires_at_ms = now_ms;
    this->state_.participants_online = online;
    return;
  }

  if (winner == nullptr) {
    if (this->state_.leader_node_id[0] != '\0') {
      copy_(this->suspended_leader_node_id_,
            sizeof(this->suspended_leader_node_id_),
            this->state_.leader_node_id);
      this->suspended_leader_term_ = this->state_.term;
    }
    this->state_.leader_node_id[0] = '\0';
    this->state_.local_leader = false;
    this->state_.lease_expires_at_ms = now_ms;
    this->state_.participants_online = online;
    return;
  }

  const uint32_t winner_term =
      winner_heartbeat != nullptr && winner_heartbeat->term_admitted
          ? winner_heartbeat->term
          : 0;
  auto term_decision = decide_role_term(
      this->state_.term, this->state_.leader_node_id,
      this->suspended_leader_term_, this->suspended_leader_node_id_, winner,
      winner_heartbeat == nullptr, winner_term,
      winner_heartbeat != nullptr && winner_heartbeat->term_admitted,
      observed_term);
  uint32_t handoff_prior_term = this->state_.term;
  if (this->suspended_leader_term_ > handoff_prior_term)
    handoff_prior_term = this->suspended_leader_term_;
  const bool relinquishing_local_standalone =
      role_id_equal(this->state_.leader_node_id, identity.node_id) ||
      role_id_equal(this->suspended_leader_node_id_, identity.node_id);
  // Equal terms are comparable only for this one-time transition from a
  // locally held standalone lease into an endorsed canonical Site contract.
  const bool verified_standalone_handoff =
      !term_decision.authority_ready && this->standalone_handoff_pending_ &&
      configuration != nullptr && this->state_.participants_expected > 1 &&
      relinquishing_local_standalone && winner_heartbeat != nullptr &&
      winner_heartbeat->term_admitted &&
      winner_heartbeat->election_contract_version >= 3 &&
      winner_heartbeat->election_view_digest == view_digest &&
      winner_heartbeat->credentials_complete &&
      exact_directory_support(winner_heartbeat->directory_revision,
                              winner_heartbeat->directory_digest) >=
          quorum.required &&
      direct_endorsements >= quorum.required &&
      this->local_credentials_complete_ && handoff_prior_term != 0 &&
      this->standalone_authority_term_ == handoff_prior_term &&
      winner_term == handoff_prior_term;
  if (verified_standalone_handoff)
    term_decision = {winner_term, true};
  if (!term_decision.authority_ready) {
    if (this->state_.leader_node_id[0] != '\0') {
      copy_(this->suspended_leader_node_id_,
            sizeof(this->suspended_leader_node_id_),
            this->state_.leader_node_id);
      this->suspended_leader_term_ = this->state_.term;
    }
    this->state_.term = term_decision.term;
    this->state_.quorum_available = false;
    this->state_.leader_node_id[0] = '\0';
    this->state_.local_leader = false;
    this->state_.lease_expires_at_ms = now_ms;
    this->state_.participants_online = online;
    return;
  }
  this->state_.term = term_decision.term;
  copy_(this->state_.leader_node_id,
        sizeof(this->state_.leader_node_id), winner);
  this->suspended_leader_node_id_[0] = '\0';
  this->suspended_leader_term_ = 0;
  this->state_.local_leader = winner_heartbeat == nullptr;
  this->state_.lease_expires_at_ms =
      winner_heartbeat == nullptr ? now_ms + DEFAULT_LEASE_MS
                                  : winner_heartbeat->expires_at_ms;
  this->state_.participants_online = online;
}

bool SiteCoordinator::valid_text_(const char *value,
                                  std::size_t max_length) {
  if (value == nullptr || value[0] == '\0')
    return false;
  for (std::size_t length = 0; length <= max_length; ++length) {
    if (value[length] == '\0')
      return true;
  }
  return false;
}

bool SiteCoordinator::time_reached_(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void SiteCoordinator::copy_(char *target, std::size_t capacity,
                            const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

const char *site_heartbeat_admission_result_to_string(
    SiteHeartbeatAdmissionResult result) {
  switch (result) {
    case SiteHeartbeatAdmissionResult::OK:
      return "ok";
    case SiteHeartbeatAdmissionResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case SiteHeartbeatAdmissionResult::UNKNOWN_PARTICIPANT:
      return "unknown_participant";
    case SiteHeartbeatAdmissionResult::PARTICIPANT_INACTIVE:
      return "participant_inactive";
    case SiteHeartbeatAdmissionResult::STALE_TERM:
      return "stale_term";
    case SiteHeartbeatAdmissionResult::TERM_SOURCE_INELIGIBLE:
      return "term_source_ineligible";
    case SiteHeartbeatAdmissionResult::EXPIRED:
      return "expired";
    case SiteHeartbeatAdmissionResult::DUPLICATE_OR_REORDERED:
      return "duplicate_or_reordered";
    case SiteHeartbeatAdmissionResult::FULL:
      return "full";
  }
  return "invalid_argument";
}

}  // namespace radar_api_server
}  // namespace esphome
