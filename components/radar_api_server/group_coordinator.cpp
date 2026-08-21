#include "group_coordinator.h"

#include "credential_provisioning.h"
#include "site_directory.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

const char *assigned_group(
    const ParticipantReadinessRecord *record,
    const SiteConfigurationSnapshot *configuration,
    const GroupMembershipView *membership,
    const char *participant_id) {
  if (membership != nullptr && membership->valid &&
      membership->contains(participant_id))
    return membership->group_id;
  if (configuration != nullptr && participant_id != nullptr) {
    const auto *member = configuration->find(participant_id);
    if (member != nullptr && member->spatial_profile.committed &&
        member->spatial_profile.group_id[0] != '\0')
      return member->spatial_profile.group_id;
  }
  return record != nullptr && ParticipantReadinessRegistry::ready(record)
             ? record->config.group_id
             : nullptr;
}

bool same_group(const ParticipantReadinessRecord *record,
                const SiteConfigurationSnapshot *configuration,
                const GroupMembershipView *membership,
                const char *participant_id, const char *group_id) {
  const char *assigned =
      assigned_group(record, configuration, membership, participant_id);
  return assigned != nullptr && std::strcmp(assigned, group_id) == 0;
}

bool participant_active(const ParticipantRecord *participant,
                        const SiteConfigurationSnapshot *configuration,
                        const GroupMembershipView *membership,
                        const char *participant_id) {
  if (participant != nullptr &&
      participant->membership == ParticipantMembership::REVOKED)
    return false;
  if (membership != nullptr && membership->valid)
    return membership->contains(participant_id);
  if (configuration != nullptr)
    return site_configuration_member_active(*configuration, participant_id);
  return participant != nullptr &&
         participant->membership == ParticipantMembership::ACTIVE;
}

bool participant_leader_eligible(
    const ParticipantRecord *participant,
    const SiteConfigurationSnapshot *configuration,
    const GroupMembershipView *membership,
    const char *participant_id) {
  if (!participant_active(participant, configuration, membership,
                          participant_id))
    return false;
  if (membership != nullptr && membership->valid) {
    const auto *member = membership->find(participant_id);
    return member != nullptr && member->leader_eligible;
  }
  if (configuration != nullptr)
    return site_configuration_member_active(*configuration, participant_id,
                                            true);
  return participant->type == ParticipantType::NAMO_NODE &&
         participant->leader_eligible;
}

}  // namespace

void GroupCoordinator::update(const PlatformIdentity &identity,
                              const ParticipantRegistry &participants,
                              const ParticipantReadinessRegistry &readiness,
                              uint32_t now_ms) {
  this->update(identity, participants, readiness, nullptr, now_ms);
}

void GroupCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration, uint32_t now_ms) {
  this->update(identity, participants, readiness, configuration, nullptr,
               now_ms);
}

void GroupCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const char *site_leader_node_id, uint32_t now_ms) {
  this->update(identity, participants, readiness, configuration, nullptr,
               nullptr, site_leader_node_id, now_ms);
}

void GroupCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantCredentialRegistry *credentials,
    const SiteDirectorySnapshot *directory,
    const char *site_leader_node_id, uint32_t now_ms) {
  this->update(identity, participants, readiness, configuration, nullptr,
               credentials, directory, site_leader_node_id, now_ms);
}

void GroupCoordinator::update(
    const PlatformIdentity &identity, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const ParticipantCredentialRegistry *credentials,
    const SiteDirectorySnapshot *directory,
    const char *site_leader_node_id, uint32_t now_ms) {
  if (!valid_text_(identity.node_id, PlatformIdentity::MAX_ID_LENGTH))
    return;
  const bool site_configuration_ready =
      configuration != nullptr && configuration->roster_committed &&
      std::strcmp(configuration->site_id, identity.site_id) == 0;
  const SiteConfigurationSnapshot *canonical =
      site_configuration_ready ? configuration : nullptr;
  const bool group_roster_ready =
      group_roster != nullptr && canonical != nullptr &&
      group_roster_current_for_local_node(*group_roster, *canonical,
                                          identity.node_id);
  const GroupMembershipView previous_membership = this->local_membership_;
  if (group_roster_ready) {
    build_local_group_membership(group_roster, identity.node_id,
                                 &this->local_membership_);
  } else {
    const bool legacy_local_ready =
        canonical != nullptr &&
        site_configuration_member_active(*canonical, identity.node_id);
    build_local_group_membership(legacy_local_ready ? canonical : nullptr,
                                 identity.node_id,
                                 &this->local_membership_);
  }
  const GroupMembershipView *canonical_membership =
      this->local_membership_.valid ? &this->local_membership_ : nullptr;
  const bool bounded_membership = this->local_membership_.valid;
  const bool membership_changed =
      std::memcmp(&previous_membership, &this->local_membership_,
                  sizeof(this->local_membership_)) != 0;

  for (auto &group : this->groups_) {
    if (!group.occupied)
      continue;
    group.seen = false;
    group.previous_expected = group.state.participants_expected;
    group.previous_online = group.state.participants_online;
    group.state.participants_expected = 0;
    group.state.participants_online = 0;
  }

  bool has_configured_group = false;
  if (bounded_membership && this->local_membership_.local_admitted) {
    GroupSlot *group =
        this->ensure_group_(this->local_membership_.group_id, true);
    if (group != nullptr) {
      group->seen = true;
      group->state.participants_expected =
          this->local_membership_.member_count;
      group->state.participants_online = 1;
      has_configured_group = true;
    }
  } else if (!bounded_membership) {
    const char *selected_group_id = nullptr;
    if (canonical != nullptr) {
      const auto *local_member = canonical->find(identity.node_id);
      if (local_member != nullptr && local_member->spatial_profile.committed &&
          local_member->spatial_profile.group_id[0] != '\0')
        selected_group_id = local_member->spatial_profile.group_id;
    }
    bool ambiguous_group = false;
    if (selected_group_id == nullptr) {
      for (std::size_t slot = 0;
           slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
        const auto *participant = participants.record_at(slot);
        const auto *profile = participant != nullptr
                                  ? readiness.find(participant->participant_id)
                                  : nullptr;
        if (participant == nullptr ||
            participant->membership != ParticipantMembership::ACTIVE ||
            !ParticipantReadinessRegistry::ready(profile))
          continue;
        const char *candidate_group = assigned_group(
            profile, canonical, nullptr, participant->participant_id);
        if (candidate_group == nullptr || candidate_group[0] == '\0')
          continue;
        if (selected_group_id == nullptr)
          selected_group_id = candidate_group;
        else if (std::strcmp(selected_group_id, candidate_group) != 0) {
          ambiguous_group = true;
          break;
        }
      }
    }
    if (ambiguous_group)
      selected_group_id = nullptr;
    for (std::size_t slot = 0;
         slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
      const auto *participant = participants.record_at(slot);
      if (participant == nullptr ||
          participant->membership != ParticipantMembership::ACTIVE)
        continue;
      if (canonical != nullptr &&
          !site_configuration_member_active(*canonical,
                                            participant->participant_id))
        continue;
      const auto *profile = readiness.find(participant->participant_id);
      if (!ParticipantReadinessRegistry::ready(profile))
        continue;
      const char *group_id =
          assigned_group(profile, canonical, canonical_membership,
                         participant->participant_id);
      if (selected_group_id == nullptr || group_id == nullptr ||
          std::strcmp(group_id, selected_group_id) != 0)
        continue;
      GroupSlot *group = this->ensure_group_(group_id, true);
      if (group == nullptr)
        continue;
      group->seen = true;
      group->state.participants_expected++;
      has_configured_group = true;
    }
  }

  if (bounded_membership && !this->local_membership_.local_admitted) {
    // A canonical eighth member is intentionally kept out of the bounded
    // high-frequency Group runtime.
  } else if (!has_configured_group) {
    GroupSlot *standalone = this->ensure_group_("", false);
    if (standalone != nullptr) {
      standalone->seen = true;
      standalone->state.participants_expected = 1;
      standalone->state.participants_online = 1;
    }
  } else if (!bounded_membership) {
    GroupSlot *local_group = nullptr;
    for (auto &group : this->groups_) {
      if (group.occupied && group.seen && group.state.configured) {
        local_group = &group;
        break;
      }
    }
    if (local_group != nullptr) {
      local_group->state.participants_expected++;
      local_group->state.participants_online++;
    }
  }

  bool changed = membership_changed;
  for (auto &group : this->groups_) {
    if (!group.occupied || group.seen)
      continue;
    group = GroupSlot{};
    changed = true;
  }

  this->reconcile_heartbeat_slots_(participants, readiness, canonical, now_ms);
  const auto *selected_local_group =
      this->local_group(canonical, identity.node_id);
  for (auto &group : this->groups_) {
    if (!group.occupied || !group.seen)
      continue;
    char previous_leader[PlatformIdentity::MAX_ID_LENGTH + 1]{};
    copy_(previous_leader, sizeof(previous_leader),
          group.state.leader_node_id);
    const uint32_t previous_term = group.state.term;
    const bool previous_local_leader = group.state.local_leader;
    const bool local_candidate_allowed =
        !group.state.configured ||
        (selected_local_group != nullptr &&
         std::strcmp(selected_local_group->group_id,
                     group.state.group_id) == 0);
    this->elect_leader_(&group, identity, participants, readiness, canonical,
                        group_roster_ready ? group_roster : nullptr,
                        credentials, directory, site_leader_node_id,
                        local_candidate_allowed, now_ms);
    group.election_roster_revision =
        canonical != nullptr ? canonical->roster_revision : 0;
    group.election_assignment_revision = group.state.assignment_revision;
    if (group.previous_expected != group.state.participants_expected ||
        group.previous_online != group.state.participants_online ||
        previous_term != group.state.term ||
        previous_local_leader != group.state.local_leader ||
        std::strcmp(previous_leader, group.state.leader_node_id) != 0)
      changed = true;
  }
  if (changed || this->generation_ == 0)
    this->generation_++;
}

GroupHeartbeatResult GroupCoordinator::accept_heartbeat(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupHeartbeat &heartbeat, uint32_t now_ms) {
  return this->accept_heartbeat(participants, readiness, nullptr, heartbeat,
                                now_ms);
}

GroupHeartbeatResult GroupCoordinator::accept_heartbeat(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const GroupHeartbeat &heartbeat, uint32_t now_ms) {
  if (!valid_text_(heartbeat.participant_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(heartbeat.group_id,
                   ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text_(heartbeat.boot_id, GroupHeartbeat::MAX_BOOT_ID_LENGTH) ||
      heartbeat.sequence == 0 || heartbeat.term == 0 ||
      heartbeat.lease_ms < MIN_LEASE_MS ||
      heartbeat.lease_ms > MAX_LEASE_MS)
    return GroupHeartbeatResult::INVALID_ARGUMENT;
  if (heartbeat.sample_age_ms >= heartbeat.lease_ms)
    return GroupHeartbeatResult::EXPIRED;

  const auto *participant = participants.find(heartbeat.participant_id);
  const bool configuration_ready =
      configuration != nullptr && configuration->roster_committed;
  const GroupMembershipView *membership =
      this->local_membership_.valid ? &this->local_membership_ : nullptr;
  if (membership != nullptr &&
      (!membership->local_admitted ||
       !membership->contains(heartbeat.participant_id)))
    return GroupHeartbeatResult::GROUP_MISMATCH;
  if (!participant_active(participant,
                          configuration_ready ? configuration : nullptr,
                          membership,
                          heartbeat.participant_id)) {
    if (!configuration_ready && participant == nullptr)
      return GroupHeartbeatResult::UNKNOWN_PARTICIPANT;
    return GroupHeartbeatResult::PARTICIPANT_INACTIVE;
  }
  if (this->local_membership_.valid &&
      (!this->local_membership_.local_admitted ||
       std::strcmp(this->local_membership_.group_id, heartbeat.group_id) != 0 ||
       !this->local_membership_.contains(heartbeat.participant_id)))
    return GroupHeartbeatResult::GROUP_MISMATCH;
  const auto *profile = readiness.find(heartbeat.participant_id);
  if (!same_group(profile, configuration_ready ? configuration : nullptr,
                  membership,
                  heartbeat.participant_id, heartbeat.group_id))
    return GroupHeartbeatResult::GROUP_MISMATCH;

  GroupSlot *group = this->find_group_(heartbeat.group_id);
  if (group == nullptr)
    return GroupHeartbeatResult::GROUP_MISMATCH;
  const bool leader_eligible = participant_leader_eligible(
      participant, configuration_ready ? configuration : nullptr,
      membership,
      heartbeat.participant_id);
  HeartbeatSlot *source = this->ensure_heartbeat_(heartbeat.participant_id);
  if (source == nullptr)
    return GroupHeartbeatResult::FULL;
  const bool same_boot = source->occupied &&
                         std::strcmp(source->boot_id, heartbeat.boot_id) == 0;
  if (same_boot && heartbeat.sequence <= source->sequence)
    return GroupHeartbeatResult::DUPLICATE_OR_REORDERED;

  const bool authority_lease_active =
      group->state.leader_node_id[0] != '\0' &&
      !time_reached_(now_ms, group->state.lease_expires_at_ms);
  const bool sender_is_current_leader =
      group->state.leader_node_id[0] != '\0' &&
      std::strcmp(group->state.leader_node_id,
                  heartbeat.participant_id) == 0;
  const bool stale_active_leader_term =
      leader_eligible && sender_is_current_leader &&
      heartbeat.term < group->state.term && authority_lease_active;
  const bool stale_term = stale_active_leader_term;

  source->occupied = true;
  copy_(source->participant_id, sizeof(source->participant_id),
        heartbeat.participant_id);
  copy_(source->boot_id, sizeof(source->boot_id), heartbeat.boot_id);
  source->sequence = heartbeat.sequence;
  source->term = heartbeat.term;
  source->term_admitted = !stale_term;
  source->roster_revision = heartbeat.roster_revision;
  source->assignment_revision = heartbeat.assignment_revision;
  source->election_view_digest = heartbeat.election_view_digest;
  source->election_contract_version = heartbeat.election_contract_version;
  source->credentials_complete = heartbeat.credentials_complete;
  source->expires_at_ms =
      now_ms + (heartbeat.lease_ms - heartbeat.sample_age_ms);
  this->generation_++;
  if (stale_term)
    return GroupHeartbeatResult::STALE_TERM;

  return GroupHeartbeatResult::OK;
}

void GroupCoordinator::clear() {
  this->groups_ = {};
  this->heartbeats_ = {};
  this->local_membership_ = {};
  this->generation_ = 0;
}

std::size_t GroupCoordinator::group_count() const {
  std::size_t count = 0;
  for (const auto &group : this->groups_) {
    if (group.occupied)
      count++;
  }
  return count;
}

const GroupCoordinatorState *GroupCoordinator::group_at(
    std::size_t slot) const {
  if (slot >= this->groups_.size() || !this->groups_[slot].occupied)
    return nullptr;
  return &this->groups_[slot].state;
}

const GroupCoordinatorState *GroupCoordinator::local_group(
    const SiteConfigurationSnapshot *configuration,
    const char *local_participant_id) const {
  if (this->local_membership_.valid) {
    if (!this->local_membership_.local_admitted ||
        local_participant_id == nullptr ||
        !this->local_membership_.contains(local_participant_id))
      return nullptr;
    for (const auto &group : this->groups_) {
      if (group.occupied &&
          std::strcmp(group.state.group_id,
                      this->local_membership_.group_id) == 0)
        return &group.state;
    }
    return nullptr;
  }

  const char *canonical_group_id = nullptr;
  if (configuration != nullptr && local_participant_id != nullptr) {
    const auto *member = configuration->find(local_participant_id);
    if (member != nullptr && member->spatial_profile.committed &&
        member->spatial_profile.group_id[0] != '\0')
      canonical_group_id = member->spatial_profile.group_id;
  }

  const GroupCoordinatorState *only_configured = nullptr;
  bool multiple_configured = false;
  for (const auto &group : this->groups_) {
    if (!group.occupied || !group.state.configured)
      continue;
    if (canonical_group_id != nullptr &&
        std::strcmp(group.state.group_id, canonical_group_id) == 0)
      return &group.state;
    if (only_configured != nullptr)
      multiple_configured = true;
    else
      only_configured = &group.state;
  }
  return canonical_group_id == nullptr && !multiple_configured
             ? only_configured
             : nullptr;
}

uint32_t GroupCoordinator::lease_remaining_ms(
    const GroupCoordinatorState &state, uint32_t now_ms) {
  if (time_reached_(now_ms, state.lease_expires_at_ms))
    return 0;
  return state.lease_expires_at_ms - now_ms;
}

GroupCoordinator::GroupSlot *GroupCoordinator::find_group_(
    const char *group_id) {
  for (auto &group : this->groups_) {
    if (group.occupied && std::strcmp(group.state.group_id, group_id) == 0)
      return &group;
  }
  return nullptr;
}

GroupCoordinator::GroupSlot *GroupCoordinator::ensure_group_(
    const char *group_id, bool configured) {
  GroupSlot *existing = this->find_group_(group_id);
  if (existing != nullptr) {
    existing->state.configured = configured;
    return existing;
  }
  for (auto &group : this->groups_) {
    if (group.occupied)
      continue;
    group = GroupSlot{};
    group.occupied = true;
    group.state.configured = configured;
    copy_(group.state.group_id, sizeof(group.state.group_id), group_id);
    return &group;
  }
  for (auto &group : this->groups_) {
    if (group.seen)
      continue;
    // update() marks the previous scope unseen before resolving the canonical
    // scope. A single-slot coordinator must replace that stale scope here,
    // before the cleanup pass that follows resolution.
    group = GroupSlot{};
    group.occupied = true;
    group.state.configured = configured;
    copy_(group.state.group_id, sizeof(group.state.group_id), group_id);
    return &group;
  }
  return nullptr;
}

GroupCoordinator::HeartbeatSlot *GroupCoordinator::find_heartbeat_(
    const char *participant_id) {
  for (auto &heartbeat : this->heartbeats_) {
    if (heartbeat.occupied &&
        std::strcmp(heartbeat.participant_id, participant_id) == 0)
      return &heartbeat;
  }
  return nullptr;
}

GroupCoordinator::HeartbeatSlot *GroupCoordinator::ensure_heartbeat_(
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

void GroupCoordinator::reconcile_heartbeat_slots_(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration, uint32_t now_ms) {
  for (auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied)
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    const auto *profile = readiness.find(heartbeat.participant_id);
    const bool canonical_assignment =
        configuration != nullptr &&
        assigned_group(profile, configuration,
                       this->local_membership_.valid
                           ? &this->local_membership_
                           : nullptr,
                       heartbeat.participant_id) !=
            nullptr;
    if (!participant_active(participant, configuration,
                            this->local_membership_.valid
                                ? &this->local_membership_
                                : nullptr,
                            heartbeat.participant_id) ||
        (this->local_membership_.valid &&
         !this->local_membership_.contains(heartbeat.participant_id)) ||
        (!canonical_assignment &&
         !ParticipantReadinessRegistry::ready(profile)) ||
        time_reached_(now_ms, heartbeat.expires_at_ms)) {
      heartbeat = HeartbeatSlot{};
    }
  }
}

void GroupCoordinator::elect_leader_(
    GroupSlot *group, const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const ParticipantCredentialRegistry *credentials,
    const SiteDirectorySnapshot *directory,
    const char *site_leader_node_id, bool local_candidate_allowed,
    uint32_t now_ms) {
  const bool prior_quorum_available = group->state.quorum_available;
  const uint32_t prior_term = group->state.term;
  const uint32_t prior_view_digest = group->state.election_view_digest;
  const uint32_t prior_compatibility_v11_view_digest =
      group->state.compatibility_v11_election_view_digest;
  const uint32_t prior_contract_view_digest =
      group->state.previous_election_view_digest;
  const uint32_t prior_legacy_view_digest =
      group->state.legacy_election_view_digest;
  const bool current_leader_lease_active =
      group->state.leader_node_id[0] != '\0' &&
      !time_reached_(now_ms, group->state.lease_expires_at_ms);
  const GroupMembershipView *membership =
      this->local_membership_.valid ? &this->local_membership_ : nullptr;
  const bool local_active =
      (membership == nullptr && configuration == nullptr) ||
      participant_active(nullptr, configuration, membership, identity.node_id);
  const auto *local_group_member =
      membership != nullptr ? membership->find(identity.node_id) : nullptr;
  const bool local_eligible =
      local_active && local_candidate_allowed &&
      (membership != nullptr
           ? local_group_member != nullptr &&
                 local_group_member->leader_eligible
           : configuration == nullptr ||
                 site_configuration_member_active(*configuration,
                                                  identity.node_id, true)) &&
      (!group->state.configured || configuration == nullptr ||
       group_leader_role_combination_allowed(
           *configuration, site_leader_node_id, identity.node_id));
  const bool local_group_credentials_complete =
      !group->state.configured || configuration == nullptr ||
      credentials == nullptr ||
      (credentials != nullptr && group_roster != nullptr
           ? group_leader_credentials_complete(identity.node_id,
                                               *group_roster, *credentials)
           : credentials != nullptr && configuration != nullptr &&
                 group_leader_credentials_complete(
                     identity.node_id, group->state.group_id,
                     *configuration, *credentials));
  const bool local_site_credentials_complete =
      !group->state.configured || configuration == nullptr ||
      credentials == nullptr ||
      site_credentials_complete_for_node(identity, *configuration, directory,
                                         site_leader_node_id, *credentials);
  const bool local_candidate_ready =
      local_eligible && local_site_credentials_complete &&
      local_group_credentials_complete;
  const char *winner =
      local_candidate_ready ? identity.node_id : nullptr;
  uint8_t winner_penalty =
      local_eligible
          ? group_leader_role_penalty(configuration, site_leader_node_id,
                                      identity.node_id)
          : 0;
  const HeartbeatSlot *winner_heartbeat = nullptr;
  bool incumbent_candidate_ready =
      configuration != nullptr && group->state.configured &&
      current_leader_lease_active &&
      local_candidate_ready &&
      std::strcmp(group->state.leader_node_id, identity.node_id) == 0;
  const HeartbeatSlot *incumbent_heartbeat = nullptr;
  uint8_t incumbent_penalty = winner_penalty;
  uint8_t online = local_active && local_candidate_allowed ? 1 : 0;
  uint32_t observed_term = group->state.term;

  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied || time_reached_(now_ms, heartbeat.expires_at_ms))
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    const auto *profile = readiness.find(heartbeat.participant_id);
    if (!participant_active(participant, configuration, membership,
                            heartbeat.participant_id) ||
        (this->local_membership_.valid &&
         !this->local_membership_.contains(heartbeat.participant_id)) ||
        !same_group(profile, configuration, membership,
                    heartbeat.participant_id,
                    group->state.group_id))
      continue;
    online++;
    const bool leader_eligible = participant_leader_eligible(
        participant, configuration, membership, heartbeat.participant_id) &&
        (!group->state.configured || configuration == nullptr ||
         group_leader_role_combination_allowed(
             *configuration, site_leader_node_id,
             heartbeat.participant_id));
    const auto *candidate_credential =
        credentials != nullptr
            ? credentials->find(heartbeat.participant_id)
            : nullptr;
    const bool candidate_credentials_available =
        credentials == nullptr ||
        (candidate_credential != nullptr && candidate_credential->enabled);
    if (!leader_eligible || !heartbeat.credentials_complete ||
        !candidate_credentials_available)
      continue;
    if (heartbeat.term_admitted && heartbeat.term > observed_term)
      observed_term = heartbeat.term;
    const uint8_t candidate_penalty =
        group_leader_role_penalty(configuration, site_leader_node_id,
                                  heartbeat.participant_id);
    if (configuration != nullptr && group->state.configured &&
        current_leader_lease_active &&
        std::strcmp(group->state.leader_node_id,
                    heartbeat.participant_id) == 0) {
      incumbent_candidate_ready = true;
      incumbent_heartbeat = &heartbeat;
      incumbent_penalty = candidate_penalty;
    }
    if (leader_candidate_preferred(heartbeat.participant_id,
                                   candidate_penalty, winner,
                                   winner_penalty)) {
      winner = heartbeat.participant_id;
      winner_penalty = candidate_penalty;
      winner_heartbeat = &heartbeat;
    }
  }

  const GroupDirectoryEntry *directory_group =
      directory != nullptr && directory->committed
          ? directory->find_group(group->state.group_id)
          : nullptr;
  const bool directory_shape_matches =
      directory_group != nullptr && directory_group->assignment_revision != 0 &&
      directory_group->member_count == group->state.participants_expected;
  bool canonical_assignment = false;
  if (directory_shape_matches) {
    const char *assigned = directory_group->leader_node_id;
    if (std::strcmp(assigned, identity.node_id) == 0) {
      if (local_candidate_ready &&
          (configuration == nullptr ||
           group_leader_assignment_allowed(
               *configuration, group->state.group_id, site_leader_node_id,
               identity.node_id))) {
        winner = identity.node_id;
        winner_heartbeat = nullptr;
        canonical_assignment = true;
      }
    } else {
      const auto *assigned_participant = participants.find(assigned);
      const auto *assigned_profile = readiness.find(assigned);
      const auto *assigned_credential =
          credentials != nullptr ? credentials->find(assigned) : nullptr;
      const HeartbeatSlot *assigned_heartbeat = nullptr;
      for (const auto &heartbeat : this->heartbeats_) {
        if (heartbeat.occupied &&
            !time_reached_(now_ms, heartbeat.expires_at_ms) &&
            std::strcmp(heartbeat.participant_id, assigned) == 0) {
          assigned_heartbeat = &heartbeat;
          break;
        }
      }
      const bool assigned_eligible =
          assigned_heartbeat != nullptr &&
          participant_leader_eligible(assigned_participant, configuration,
                                      membership,
                                      assigned) &&
          (configuration == nullptr ||
           group_leader_assignment_allowed(
               *configuration, group->state.group_id, site_leader_node_id,
               assigned)) &&
          same_group(assigned_profile, configuration, membership, assigned,
                     group->state.group_id) &&
          assigned_heartbeat->credentials_complete &&
          (credentials == nullptr ||
           (assigned_credential != nullptr && assigned_credential->enabled));
      if (assigned_eligible) {
        winner = assigned;
        winner_heartbeat = assigned_heartbeat;
        canonical_assignment = true;
      }
    }
  }
  const bool incumbent_selected =
      incumbent_candidate_ready && incumbent_penalty < 2;
  if (incumbent_selected) {
    winner = group->state.leader_node_id;
    winner_penalty = incumbent_penalty;
    winner_heartbeat = incumbent_heartbeat;
    canonical_assignment =
        directory_shape_matches &&
        std::strcmp(directory_group->leader_node_id, winner) == 0;
  }

  group->state.assignment_revision =
      directory_shape_matches ? directory_group->assignment_revision : 0;
  group->state.canonical_assignment = canonical_assignment;

  const uint64_t assignment_qualifier =
      directory_shape_matches
          ? (static_cast<uint64_t>(directory_group->assignment_revision)
                 << 32U) |
                group_directory_assignment_hash(*directory_group)
          : 0;
  RoleElectionView observed_election_view{};
  RoleElectionView contract_election_view{};
  if (local_active && local_candidate_allowed)
    observed_election_view.add(identity.node_id, assignment_qualifier,
                               local_site_credentials_complete);
  if (membership != nullptr && group->state.configured) {
    // Group membership is bounded to seven canonical members. Build the
    // modern election contract from that fixed roster; heartbeat arrival only
    // contributes liveness and endorsements.
    const uint64_t roster_qualifier =
        configuration != nullptr ? configuration->roster_revision : 0;
    for (const auto &member : membership->members) {
      if (!member.occupied)
        continue;
      contract_election_view.add(member.participant_id, roster_qualifier,
                                 true);
    }

    for (const auto &heartbeat : this->heartbeats_) {
      if (!heartbeat.occupied ||
          time_reached_(now_ms, heartbeat.expires_at_ms))
        continue;
      const auto *participant = participants.find(heartbeat.participant_id);
      const auto *profile = readiness.find(heartbeat.participant_id);
      if (!participant_active(participant, configuration, membership,
                              heartbeat.participant_id) ||
          (this->local_membership_.valid &&
           !this->local_membership_.contains(heartbeat.participant_id)) ||
          !same_group(profile, configuration, membership,
                      heartbeat.participant_id, group->state.group_id))
        continue;
      observed_election_view.add(heartbeat.participant_id,
                                 assignment_qualifier,
                                 heartbeat.credentials_complete);
    }

    if (!incumbent_selected && group->state.participants_expected > 1) {
      const uint8_t required =
          strict_majority(group->state.participants_expected);
      uint32_t endorsed_digest = 0;
      uint8_t endorsed_contract_version = 0;
      for (const uint8_t contract_version : {3U, 2U}) {
        for (const auto &candidate_vote : this->heartbeats_) {
          if (!candidate_vote.occupied ||
              time_reached_(now_ms, candidate_vote.expires_at_ms) ||
              candidate_vote.election_contract_version != contract_version ||
              candidate_vote.election_view_digest == 0 ||
              !candidate_vote.credentials_complete ||
              !membership->contains(candidate_vote.participant_id))
            continue;
          uint8_t support = 0;
          for (const auto &vote : this->heartbeats_) {
            if (vote.occupied &&
                !time_reached_(now_ms, vote.expires_at_ms) &&
                vote.election_contract_version == contract_version &&
                vote.credentials_complete &&
                vote.election_view_digest ==
                    candidate_vote.election_view_digest &&
                membership->contains(vote.participant_id))
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
        for (const auto &member : membership->members) {
          if (!member.occupied || !member.leader_eligible ||
              group_leader_role_penalty(configuration, site_leader_node_id,
                                        member.participant_id) >= 2 ||
              (endorsed_contract_version >= 3
                   ? contract_election_view
                   : observed_election_view)
                      .winner_contract_digest(group->state.group_id,
                                              member.participant_id) !=
                  endorsed_digest)
            continue;
          if (std::strcmp(member.participant_id, identity.node_id) == 0) {
            if (!local_candidate_ready)
              continue;
            winner = identity.node_id;
            winner_penalty = group_leader_role_penalty(
                configuration, site_leader_node_id, winner);
            winner_heartbeat = nullptr;
            break;
          }

          const auto *participant = participants.find(member.participant_id);
          const auto *profile = readiness.find(member.participant_id);
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
          const auto *credential =
              credentials != nullptr
                  ? credentials->find(member.participant_id)
                  : nullptr;
          if (endorsed_winner == nullptr ||
              !participant_leader_eligible(participant, configuration,
                                           membership,
                                           member.participant_id) ||
              !group_leader_role_combination_allowed(
                  *configuration, site_leader_node_id,
                  member.participant_id) ||
              !same_group(profile, configuration, membership,
                          member.participant_id, group->state.group_id) ||
              (credentials != nullptr &&
               (credential == nullptr || !credential->enabled)))
            continue;
          winner = endorsed_winner->participant_id;
          winner_penalty = group_leader_role_penalty(
              configuration, site_leader_node_id, winner);
          winner_heartbeat = endorsed_winner;
          break;
        }
        canonical_assignment =
            directory_shape_matches && winner != nullptr &&
            std::strcmp(directory_group->leader_node_id, winner) == 0;
      }
    }
  }
  group->state.canonical_assignment = canonical_assignment;
  group->state.credentials_complete =
      winner != nullptr &&
      ((winner_heartbeat == nullptr && local_group_credentials_complete) ||
       (winner_heartbeat != nullptr && winner_heartbeat->credentials_complete &&
        credentials != nullptr && credentials->find(winner) != nullptr));
  bool remote_fence_observed = false;
  for (const auto &heartbeat : this->heartbeats_) {
    if (!heartbeat.occupied || time_reached_(now_ms, heartbeat.expires_at_ms))
      continue;
    const auto *participant = participants.find(heartbeat.participant_id);
    const auto *profile = readiness.find(heartbeat.participant_id);
    if (!participant_active(participant, configuration, membership,
                            heartbeat.participant_id) ||
        (this->local_membership_.valid &&
         !this->local_membership_.contains(heartbeat.participant_id)) ||
        !same_group(profile, configuration, membership,
                    heartbeat.participant_id, group->state.group_id))
      continue;
    if (membership == nullptr || !group->state.configured)
      observed_election_view.add(heartbeat.participant_id,
                                 assignment_qualifier,
                                 heartbeat.credentials_complete);
    if (heartbeat.election_view_digest != 0)
      remote_fence_observed = true;
  }

  const uint32_t previous_view_digest =
      observed_election_view.contract_digest(group->state.group_id);
  const uint32_t view_digest =
      (membership != nullptr && group->state.configured
           ? contract_election_view
           : observed_election_view)
          .winner_contract_digest(group->state.group_id, winner);
  const uint32_t compatibility_v11_view_digest =
      observed_election_view.winner_contract_digest(group->state.group_id,
                                                     winner);
  const uint32_t legacy_view_digest =
      observed_election_view.digest(group->state.group_id);
  const bool incumbent_transition_allowed =
      prior_quorum_available && prior_term != 0 && incumbent_selected &&
      role_id_equal(group->state.leader_node_id, winner) &&
      observed_term <= prior_term;
  const bool local_roster_transition =
      configuration != nullptr && group->election_roster_revision != 0 &&
      configuration->roster_revision != group->election_roster_revision;
  const bool local_assignment_transition =
      group->election_assignment_revision != 0 &&
      group->state.assignment_revision != 0 &&
      group->state.assignment_revision != group->election_assignment_revision;
  const bool remote_incumbent_revision_transition =
      incumbent_heartbeat != nullptr && configuration != nullptr &&
      ((incumbent_heartbeat->roster_revision != 0 &&
        incumbent_heartbeat->roster_revision !=
            configuration->roster_revision) ||
       (incumbent_heartbeat->assignment_revision != 0 &&
        group->state.assignment_revision != 0 &&
        incumbent_heartbeat->assignment_revision !=
            group->state.assignment_revision));
  bool remote_revision_transition_observed = false;
  if (configuration != nullptr) {
    for (const auto &heartbeat : this->heartbeats_) {
      if (!heartbeat.occupied ||
          time_reached_(now_ms, heartbeat.expires_at_ms) ||
          !heartbeat.credentials_complete)
        continue;
      const bool roster_differs =
          heartbeat.roster_revision != 0 &&
          heartbeat.roster_revision != configuration->roster_revision;
      const bool assignment_differs =
          heartbeat.assignment_revision != 0 &&
          group->state.assignment_revision != 0 &&
          heartbeat.assignment_revision != group->state.assignment_revision;
      if (roster_differs || assignment_differs) {
        remote_revision_transition_observed = true;
        break;
      }
    }
  }
  const bool bounded_incumbent_transition =
      incumbent_transition_allowed &&
      (local_roster_transition || local_assignment_transition ||
       remote_incumbent_revision_transition ||
       remote_revision_transition_observed ||
       group->transition_views.active());
  const bool election_contract_changed =
      view_digest != prior_view_digest ||
      compatibility_v11_view_digest !=
          prior_compatibility_v11_view_digest ||
      previous_view_digest != prior_contract_view_digest ||
      legacy_view_digest != prior_legacy_view_digest;
  if (!bounded_incumbent_transition) {
    group->transition_views.reset();
  } else if (election_contract_changed) {
    group->transition_views.capture(
        prior_view_digest, prior_compatibility_v11_view_digest,
        prior_contract_view_digest, prior_legacy_view_digest);
  }
  uint8_t endorsements = local_active && local_candidate_allowed ? 1 : 0;
  uint8_t transition_view_endorsements = 0;
  BoundedRoleElectionDigestVotes<
      ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      alternate_revision_votes{};
  const bool incumbent_remote_transition_available =
      bounded_incumbent_transition && remote_incumbent_revision_transition &&
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
                  : heartbeat.election_contract_version == 1
                        ? previous_view_digest
                        : legacy_view_digest;
    const bool endorses_current =
        heartbeat.election_view_digest == expected_digest;
    const bool endorses_transition =
        !endorses_current && group->transition_views.matches(
                                  heartbeat.election_contract_version,
                                  heartbeat.election_view_digest);
    const bool endorses_incumbent_transition =
        !endorses_current && !endorses_transition &&
        incumbent_remote_transition_available &&
        heartbeat.election_contract_version ==
            incumbent_heartbeat->election_contract_version &&
        heartbeat.election_view_digest ==
            incumbent_heartbeat->election_view_digest;
    const auto *participant = participants.find(heartbeat.participant_id);
    const auto *profile = readiness.find(heartbeat.participant_id);
    if (!participant_active(participant, configuration, membership,
                            heartbeat.participant_id) ||
        (this->local_membership_.valid &&
         !this->local_membership_.contains(heartbeat.participant_id)) ||
        !same_group(profile, configuration, membership,
                    heartbeat.participant_id, group->state.group_id))
      continue;
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
      group->state.participants_expected, online);
  const bool fence_supported =
      configuration != nullptr && group->state.configured &&
      group->state.participants_expected > 1 &&
      (directory_shape_matches || remote_fence_observed);
  const uint8_t alternate_transition_endorsements =
      alternate_revision_votes.strongest();
  if (alternate_transition_endorsements > transition_view_endorsements)
    transition_view_endorsements = alternate_transition_endorsements;
  if (endorsements >= quorum.required)
    group->transition_views.reset();
  const bool canonical_transition_assignment =
      canonical_assignment || bounded_incumbent_transition;
  if (fence_supported && canonical_transition_assignment &&
      group->state.credentials_complete)
    endorsements = preserve_incumbent_transition_endorsements(
        prior_quorum_available, group->state.leader_node_id, prior_term,
        observed_term, winner, endorsements, transition_view_endorsements,
        quorum.required);
  const bool fence_available =
      !fence_supported || endorsements >= quorum.required;
  group->state.quorum_required = quorum.required;
  group->state.quorum_available = quorum.available && fence_available;
  group->state.election_view_digest = view_digest;
  group->state.compatibility_v11_election_view_digest =
      compatibility_v11_view_digest;
  group->state.previous_election_view_digest = previous_view_digest;
  group->state.legacy_election_view_digest = legacy_view_digest;
  group->state.election_endorsements = endorsements;
  group->state.election_fenced = fence_supported;
  if (configuration != nullptr && group->state.configured &&
      group->state.participants_expected > 1 &&
      !group->state.quorum_available) {
    if (group->state.term == 0)
      group->state.term = 1;
    if (group->state.leader_node_id[0] != '\0') {
      copy_(group->suspended_leader_node_id,
            sizeof(group->suspended_leader_node_id),
            group->state.leader_node_id);
      group->suspended_leader_term = group->state.term;
    }
    group->state.leader_node_id[0] = '\0';
    group->state.local_leader = false;
    group->state.lease_expires_at_ms = now_ms;
    group->state.participants_online = online;
    group->state.credentials_complete = false;
    group->state.canonical_assignment = false;
    return;
  }

  if (winner == nullptr) {
    if (group->state.leader_node_id[0] != '\0') {
      copy_(group->suspended_leader_node_id,
            sizeof(group->suspended_leader_node_id),
            group->state.leader_node_id);
      group->suspended_leader_term = group->state.term;
    }
    group->state.leader_node_id[0] = '\0';
    group->state.local_leader = false;
    group->state.lease_expires_at_ms = now_ms;
    group->state.participants_online = online;
    group->state.credentials_complete = false;
    group->state.canonical_assignment = false;
    return;
  }

  const uint32_t winner_term =
      winner_heartbeat != nullptr && winner_heartbeat->term_admitted
          ? winner_heartbeat->term
          : 0;
  const auto term_decision = decide_role_term(
      group->state.term, group->state.leader_node_id,
      group->suspended_leader_term, group->suspended_leader_node_id, winner,
      winner_heartbeat == nullptr, winner_term,
      winner_heartbeat != nullptr && winner_heartbeat->term_admitted,
      observed_term);
  if (!term_decision.authority_ready) {
    if (group->state.leader_node_id[0] != '\0') {
      copy_(group->suspended_leader_node_id,
            sizeof(group->suspended_leader_node_id),
            group->state.leader_node_id);
      group->suspended_leader_term = group->state.term;
    }
    group->state.term = term_decision.term;
    group->state.quorum_available = false;
    group->state.leader_node_id[0] = '\0';
    group->state.local_leader = false;
    group->state.lease_expires_at_ms = now_ms;
    group->state.participants_online = online;
    group->state.credentials_complete = false;
    group->state.canonical_assignment = false;
    return;
  }
  group->state.term = term_decision.term;
  copy_(group->state.leader_node_id,
        sizeof(group->state.leader_node_id), winner);
  group->suspended_leader_node_id[0] = '\0';
  group->suspended_leader_term = 0;
  group->state.local_leader = winner_heartbeat == nullptr;
  group->state.lease_expires_at_ms =
      winner_heartbeat == nullptr ? now_ms + DEFAULT_LEASE_MS
                                  : winner_heartbeat->expires_at_ms;
  group->state.participants_online = online;
}

bool GroupCoordinator::valid_text_(const char *value, std::size_t max_length) {
  if (value == nullptr || value[0] == '\0')
    return false;
  for (std::size_t length = 0; length <= max_length; ++length) {
    if (value[length] == '\0')
      return true;
  }
  return false;
}

bool GroupCoordinator::time_reached_(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void GroupCoordinator::copy_(char *target, std::size_t capacity,
                             const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

const char *group_heartbeat_result_to_string(GroupHeartbeatResult result) {
  switch (result) {
    case GroupHeartbeatResult::OK:
      return "ok";
    case GroupHeartbeatResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case GroupHeartbeatResult::UNKNOWN_PARTICIPANT:
      return "unknown_participant";
    case GroupHeartbeatResult::PARTICIPANT_INACTIVE:
      return "participant_inactive";
    case GroupHeartbeatResult::GROUP_MISMATCH:
      return "group_mismatch";
    case GroupHeartbeatResult::STALE_TERM:
      return "stale_term";
    case GroupHeartbeatResult::TERM_SOURCE_INELIGIBLE:
      return "term_source_ineligible";
    case GroupHeartbeatResult::EXPIRED:
      return "expired";
    case GroupHeartbeatResult::DUPLICATE_OR_REORDERED:
      return "duplicate_or_reordered";
    case GroupHeartbeatResult::FULL:
      return "full";
  }
  return "invalid_argument";
}

}  // namespace radar_api_server
}  // namespace esphome
