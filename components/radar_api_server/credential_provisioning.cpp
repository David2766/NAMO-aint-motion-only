#include "credential_provisioning.h"

#include "site_directory.h"
#include "site_heartbeat.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t CREDENTIAL_PROVISIONING_MIN_HEARTBEAT_VERSION = 8;

static_assert(ParticipantRegistry::MAX_PARTICIPANTS <= 32,
              "credential source attempt mask must cover every member slot");

bool active_member(const SiteConfigurationSnapshot &configuration,
                   const char *participant_id) {
  return participant_id != nullptr && participant_id[0] != '\0' &&
         site_configuration_member_active(configuration, participant_id);
}

const char *member_group(const SiteConfigurationSnapshot &configuration,
                         const char *participant_id) {
  const auto *member = configuration.find(participant_id);
  return member != nullptr && member->spatial_profile.committed &&
                 member->spatial_profile.group_id[0] != '\0'
             ? member->spatial_profile.group_id
             : nullptr;
}

bool directory_group_leader(const SiteDirectorySnapshot *directory,
                            const char *participant_id) {
  if (directory == nullptr || participant_id == nullptr)
    return false;
  for (const auto &group : directory->groups) {
    if (group.occupied &&
        std::strcmp(group.leader_node_id, participant_id) == 0)
      return true;
  }
  return false;
}

bool same_record(const ParticipantCredentialRecord &left,
                 const ParticipantCredentialRecord &right) {
  return left.enabled == right.enabled &&
         left.participant_token == right.participant_token &&
         left.key_version == right.key_version && left.key == right.key;
}

}  // namespace

const char *credential_provisioning_result_to_string(
    CredentialProvisioningResult result) {
  switch (result) {
    case CredentialProvisioningResult::OK: return "ok";
    case CredentialProvisioningResult::ALREADY_COMPLETE:
      return "already_complete";
    case CredentialProvisioningResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case CredentialProvisioningResult::NOT_OWNER: return "not_owner";
    case CredentialProvisioningResult::REQUESTER_INACTIVE:
      return "requester_inactive";
    case CredentialProvisioningResult::SOURCE_CREDENTIAL_MISSING:
      return "source_credential_missing";
    case CredentialProvisioningResult::INVALID_BUNDLE:
      return "invalid_bundle";
    case CredentialProvisioningResult::REGISTRY_FULL: return "registry_full";
  }
  return "invalid_argument";
}

const char *credential_provisioning_source_readiness_to_string(
    CredentialProvisioningSourceReadiness readiness) {
  switch (readiness) {
    case CredentialProvisioningSourceReadiness::READY: return "ready";
    case CredentialProvisioningSourceReadiness::ENDPOINT_MISSING:
      return "source_endpoint_missing";
    case CredentialProvisioningSourceReadiness::ENDPOINT_UNAUTHENTICATED:
      return "source_endpoint_unauthenticated";
    case CredentialProvisioningSourceReadiness::ENDPOINT_ADDRESS_MISSING:
      return "source_address_missing";
    case CredentialProvisioningSourceReadiness::ENDPOINT_OFFLINE:
      return "source_offline";
    case CredentialProvisioningSourceReadiness::LEGACY_HEARTBEAT:
      return "source_heartbeat_legacy";
    case CredentialProvisioningSourceReadiness::AUTHORITY_EPOCH_MISMATCH:
      return "source_authority_epoch_mismatch";
    case CredentialProvisioningSourceReadiness::CONFIGURATION_GENERATION_MISMATCH:
      return "source_configuration_generation_mismatch";
    case CredentialProvisioningSourceReadiness::CONFIGURATION_DIGEST_MISMATCH:
      return "source_configuration_digest_mismatch";
    case CredentialProvisioningSourceReadiness::TOPOLOGY_REVISION_MISMATCH:
      return "source_topology_revision_mismatch";
    case CredentialProvisioningSourceReadiness::TOPOLOGY_DIGEST_MISMATCH:
      return "source_topology_digest_mismatch";
  }
  return "source_endpoint_missing";
}

CredentialProvisioningSourceReadiness
credential_provisioning_source_readiness(
    const ParticipantEndpoint *source_endpoint,
    const SiteConfigurationSnapshot &configuration, uint32_t now_ms) {
  if (source_endpoint == nullptr)
    return CredentialProvisioningSourceReadiness::ENDPOINT_MISSING;
  if (!source_endpoint->authenticated)
    return CredentialProvisioningSourceReadiness::ENDPOINT_UNAUTHENTICATED;
  if (source_endpoint->ipv4 == 0)
    return CredentialProvisioningSourceReadiness::ENDPOINT_ADDRESS_MISSING;
  if (!ParticipantEndpointCache::is_online(source_endpoint, now_ms))
    return CredentialProvisioningSourceReadiness::ENDPOINT_OFFLINE;
  if (source_endpoint->heartbeat_wire_version <
      CREDENTIAL_PROVISIONING_MIN_HEARTBEAT_VERSION)
    return CredentialProvisioningSourceReadiness::LEGACY_HEARTBEAT;
  if (source_endpoint->site_configuration_authority_epoch !=
      configuration.authority_epoch)
    return CredentialProvisioningSourceReadiness::AUTHORITY_EPOCH_MISMATCH;
  if (source_endpoint->site_configuration_generation !=
      configuration.generation)
    return CredentialProvisioningSourceReadiness::
        CONFIGURATION_GENERATION_MISMATCH;
  if (source_endpoint->site_configuration_digest !=
      site_configuration_digest(configuration))
    return CredentialProvisioningSourceReadiness::
        CONFIGURATION_DIGEST_MISMATCH;
  if (source_endpoint->site_topology_revision != configuration.roster_revision)
    return CredentialProvisioningSourceReadiness::TOPOLOGY_REVISION_MISMATCH;
  if (source_endpoint->site_topology_digest !=
      site_topology_digest(configuration))
    return CredentialProvisioningSourceReadiness::TOPOLOGY_DIGEST_MISMATCH;
  return CredentialProvisioningSourceReadiness::READY;
}

bool credential_provisioning_authority_bootstrap_source_ready(
    const ParticipantEndpoint *source_endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *source_participant_id) {
  if (source_endpoint == nullptr || source_participant_id == nullptr ||
      source_participant_id[0] == '\0' || source_endpoint->ipv4 == 0 ||
      std::strcmp(source_endpoint->participant_id, source_participant_id) != 0)
    return false;
  const auto *member = configuration.find(source_participant_id);
  return configuration.roster_committed && member != nullptr &&
         member->type == ParticipantType::NAMO_NODE &&
         member->membership == ParticipantMembership::ACTIVE;
}

CredentialReconciliationSummary reconcile_site_credential_registry(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    ParticipantCredentialRegistry *credentials) {
  CredentialReconciliationSummary summary{};
  if (credentials == nullptr || !configuration.roster_committed)
    return summary;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *credential = credentials->record_at(slot);
    if (credential == nullptr)
      continue;
    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    std::strncpy(participant_id, credential->participant_id,
                 ParticipantRecord::MAX_ID_LENGTH);
    const auto *canonical = configuration.find(participant_id);
    const auto *direct = participants.find(participant_id);
    if (canonical == nullptr && direct == nullptr) {
      if (credentials->erase(participant_id) == ParticipantCredentialResult::OK)
        ++summary.erased_orphans;
      continue;
    }
    const auto membership = canonical != nullptr ? canonical->membership
                                                 : direct->membership;
    if (canonical != nullptr)
      ++summary.preserved_canonical;
    if (membership == ParticipantMembership::REVOKED) {
      if (credential->enabled &&
          credentials->set_enabled(participant_id, false) ==
              ParticipantCredentialResult::OK)
        ++summary.disabled_revoked;
    }
  }
  return summary;
}

uint32_t credential_provisioning_source_bit(
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id) {
  if (participant_id == nullptr || participant_id[0] == '\0')
    return 0;
  for (std::size_t slot = 0; slot < configuration.members.size(); ++slot) {
    const auto &member = configuration.members[slot];
    if (member.occupied &&
        std::strcmp(member.participant_id, participant_id) == 0)
      return static_cast<uint32_t>(1UL << slot);
  }
  return 0;
}

const ParticipantEndpoint *select_credential_provisioning_source(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials,
    const char *preferred_fallback_node_id,
    uint32_t attempted_source_mask, uint32_t now_ms,
    bool allow_authority_bootstrap) {
  const auto candidate_available = [&](const ParticipantEndpoint *endpoint) {
    if (endpoint == nullptr || endpoint->participant_id[0] == '\0' ||
        std::strcmp(endpoint->participant_id, identity.node_id) == 0)
      return false;
    const auto *member = configuration.find(endpoint->participant_id);
    const uint32_t source_bit = credential_provisioning_source_bit(
        configuration, endpoint->participant_id);
    if (member == nullptr || member->type != ParticipantType::NAMO_NODE ||
        member->membership != ParticipantMembership::ACTIVE ||
        source_bit == 0 || (attempted_source_mask & source_bit) != 0)
      return false;
    const auto *credential = credentials.find(endpoint->participant_id);
    return credential != nullptr && credential->enabled;
  };
  const auto exact = [&](const ParticipantEndpoint *endpoint) {
    return candidate_available(endpoint) &&
           credential_provisioning_source_readiness(
               endpoint, configuration, now_ms) ==
               CredentialProvisioningSourceReadiness::READY;
  };
  const auto bootstrap = [&](const ParticipantEndpoint *endpoint) {
    return allow_authority_bootstrap && candidate_available(endpoint) &&
           credential_provisioning_authority_bootstrap_source_ready(
               endpoint, configuration, endpoint->participant_id);
  };
  const auto *owner =
      endpoints.find(configuration.configuration_owner_node_id);
  if (exact(owner))
    return owner;
  if (preferred_fallback_node_id != nullptr &&
      preferred_fallback_node_id[0] != '\0') {
    const auto *preferred = endpoints.find(preferred_fallback_node_id);
    if (exact(preferred))
      return preferred;
  }
  const ParticipantEndpoint *selected_complete = nullptr;
  const ParticipantEndpoint *selected_fallback = nullptr;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *candidate = endpoints.endpoint_at(slot);
    if (!exact(candidate))
      continue;
    auto **selected = candidate->site_credentials_complete
                          ? &selected_complete
                          : &selected_fallback;
    if (*selected == nullptr ||
        std::strcmp(candidate->participant_id,
                    (*selected)->participant_id) < 0)
      *selected = candidate;
  }
  if (selected_complete != nullptr || selected_fallback != nullptr)
    return selected_complete != nullptr ? selected_complete : selected_fallback;

  if (bootstrap(owner))
    return owner;
  if (preferred_fallback_node_id != nullptr &&
      preferred_fallback_node_id[0] != '\0') {
    const auto *preferred = endpoints.find(preferred_fallback_node_id);
    if (bootstrap(preferred))
      return preferred;
  }
  const ParticipantEndpoint *selected_bootstrap = nullptr;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *candidate = endpoints.endpoint_at(slot);
    if (!bootstrap(candidate))
      continue;
    if (selected_bootstrap == nullptr ||
        std::strcmp(candidate->participant_id,
                    selected_bootstrap->participant_id) < 0)
      selected_bootstrap = candidate;
  }
  return selected_bootstrap;
}

CredentialProvisioningContextChange credential_provisioning_context_change(
    uint32_t previous_authority_epoch, uint32_t previous_roster_revision,
    const char *previous_owner_node_id,
    const SiteConfigurationSnapshot &configuration) {
  if (previous_owner_node_id == nullptr ||
      previous_authority_epoch != configuration.authority_epoch ||
      std::strcmp(previous_owner_node_id,
                  configuration.configuration_owner_node_id) != 0)
    return CredentialProvisioningContextChange::AUTHORITY_REFRESH;
  if (previous_roster_revision != configuration.roster_revision)
    return CredentialProvisioningContextChange::ROSTER_REFRESH;
  return CredentialProvisioningContextChange::NONE;
}

bool site_credential_required_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const char *candidate_participant_id) {
  if (!configuration.roster_committed ||
      !active_member(configuration, identity.node_id) ||
      !active_member(configuration, candidate_participant_id) ||
      std::strcmp(identity.node_id, candidate_participant_id) == 0)
    return false;

  if (std::strcmp(candidate_participant_id,
                  configuration.configuration_owner_node_id) == 0)
    return true;

  const auto *candidate = configuration.find(candidate_participant_id);
  if (candidate != nullptr && candidate->type == ParticipantType::NAMO_NODE &&
      candidate->leader_eligible)
    return true;

  const char *local_group = member_group(configuration, identity.node_id);
  const char *candidate_group =
      member_group(configuration, candidate_participant_id);
  if (local_group != nullptr && candidate_group != nullptr &&
      std::strcmp(local_group, candidate_group) == 0)
    return true;

  const bool local_site_leader =
      site_leader_node_id != nullptr && site_leader_node_id[0] != '\0' &&
      std::strcmp(identity.node_id, site_leader_node_id) == 0;
  const bool candidate_site_leader =
      site_leader_node_id != nullptr && site_leader_node_id[0] != '\0' &&
      std::strcmp(candidate_participant_id, site_leader_node_id) == 0;
  const bool local_group_leader =
      directory_group_leader(directory, identity.node_id);
  const bool candidate_group_leader =
      directory_group_leader(directory, candidate_participant_id);
  if ((local_site_leader && candidate_group_leader) ||
      (local_group_leader && candidate_site_leader))
    return true;

  const auto *successor = automatic_configuration_successor(configuration);
  return successor != nullptr &&
         std::strcmp(successor->participant_id, identity.node_id) == 0;
}

bool site_credentials_complete_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &credentials) {
  if (!configuration.roster_committed ||
      !active_member(configuration, identity.node_id))
    return false;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        !site_credential_required_for_node(
            identity, configuration, directory, site_leader_node_id,
            member.participant_id))
      continue;
    const auto *credential = credentials.find(member.participant_id);
    if (credential == nullptr || !credential->enabled)
      return false;
  }
  return true;
}

bool site_credential_bootstrap_ready_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials) {
  if (!configuration.roster_committed || identity.node_id[0] == '\0' ||
      configuration.configuration_owner_node_id[0] == '\0' ||
      std::strcmp(identity.node_id,
                  configuration.configuration_owner_node_id) == 0)
    return false;

  const auto *local = configuration.find(identity.node_id);
  const auto *owner =
      configuration.find(configuration.configuration_owner_node_id);
  const auto *direct_pair =
      participants.find(configuration.configuration_owner_node_id);
  const auto *owner_credential =
      credentials.find(configuration.configuration_owner_node_id);
  return local != nullptr && local->type == ParticipantType::NAMO_NODE &&
         local->membership == ParticipantMembership::QUARANTINED &&
         owner != nullptr && owner->type == ParticipantType::NAMO_NODE &&
         owner->membership == ParticipantMembership::ACTIVE &&
         direct_pair != nullptr &&
         direct_pair->type == ParticipantType::NAMO_NODE &&
         direct_pair->membership != ParticipantMembership::REVOKED &&
         owner_credential != nullptr && owner_credential->enabled;
}

bool group_leader_credentials_complete(
    const char *leader_node_id, const char *group_id,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantCredentialRegistry &credentials) {
  if (!active_member(configuration, leader_node_id) || group_id == nullptr ||
      group_id[0] == '\0')
    return false;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        std::strcmp(member.participant_id, leader_node_id) == 0 ||
        !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.group_id, group_id) != 0)
      continue;
    const auto *credential = credentials.find(member.participant_id);
    if (credential == nullptr || !credential->enabled)
      return false;
  }
  return true;
}

bool group_leader_credentials_complete(
    const char *leader_node_id, const GroupRosterSnapshot &roster,
    const ParticipantCredentialRegistry &credentials) {
  const auto *leader = roster.find(leader_node_id);
  if (!valid_group_roster(roster) || leader == nullptr ||
      !leader->leader_eligible)
    return false;
  for (const auto &member : roster.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, leader_node_id) == 0)
      continue;
    const auto *credential = credentials.find(member.participant_id);
    if (credential == nullptr || !credential->enabled)
      return false;
  }
  return true;
}

const ParticipantCredentialRecord *select_site_sender_credential(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantCredentialRegistry &credentials,
    const char *target_participant_id) {
  if (target_participant_id == nullptr || target_participant_id[0] == '\0')
    return nullptr;
  if (configuration != nullptr && configuration->roster_committed &&
      configuration->configuration_owner_node_id[0] != '\0' &&
      std::strcmp(identity.node_id,
                  configuration->configuration_owner_node_id) != 0)
    return credentials.find(configuration->configuration_owner_node_id);
  return credentials.find(target_participant_id);
}

const ParticipantCredentialRecord *credential_probe_at(
    const ParticipantCredentialRegistry &credentials, std::size_t index) {
  std::size_t occupied = 0;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *record = credentials.record_at(slot);
    if (record == nullptr || !record->enabled)
      continue;
    if (occupied++ == index)
      return record;
  }
  return nullptr;
}

bool credential_bundle_binds_requester(
    const ParticipantCredentialRegistry &bundle,
    const char *configuration_owner_node_id,
    const ParticipantCredentialRecord &request_credential) {
  if (configuration_owner_node_id == nullptr ||
      configuration_owner_node_id[0] == '\0' ||
      !request_credential.enabled)
    return false;
  const auto *owner = bundle.find(configuration_owner_node_id);
  return owner != nullptr && owner->enabled &&
         owner->participant_token == request_credential.participant_token &&
         owner->key_version == request_credential.key_version &&
         owner->key == request_credential.key;
}

CredentialProvisioningResult prepare_site_credential_bundle(
    const PlatformIdentity &owner_identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &owner_credentials,
    const char *requester_participant_id,
    ParticipantCredentialRegistry *bundle) {
  if (bundle == nullptr || requester_participant_id == nullptr ||
      requester_participant_id[0] == '\0' ||
      !configuration.roster_committed)
    return CredentialProvisioningResult::INVALID_ARGUMENT;
  if (!active_member(configuration, owner_identity.node_id))
    return CredentialProvisioningResult::NOT_OWNER;
  if (!active_member(configuration, requester_participant_id))
    return CredentialProvisioningResult::REQUESTER_INACTIVE;

  bundle->clear();
  PlatformIdentity requester_identity = owner_identity;
  std::strncpy(requester_identity.node_id, requester_participant_id,
               PlatformIdentity::MAX_ID_LENGTH);
  requester_identity.node_id[PlatformIdentity::MAX_ID_LENGTH] = '\0';
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        !site_credential_required_for_node(
            requester_identity, configuration, directory,
            site_leader_node_id, member.participant_id))
      continue;
    const bool current_owner_record =
        std::strcmp(member.participant_id,
                    configuration.configuration_owner_node_id) == 0;
    const bool custodian_record =
        !current_owner_record &&
        std::strcmp(owner_identity.node_id,
                    configuration.configuration_owner_node_id) != 0 &&
        std::strcmp(member.participant_id, owner_identity.node_id) == 0;
    const char *source_record_id =
        current_owner_record
            ? requester_participant_id
            : (custodian_record
                   ? configuration.configuration_owner_node_id
                   : member.participant_id);
    const auto *source = owner_credentials.find(source_record_id);
    if (source == nullptr || !source->enabled) {
      bundle->clear();
      return CredentialProvisioningResult::SOURCE_CREDENTIAL_MISSING;
    }
    const auto result = bundle->set(
        member.participant_id, source->participant_token,
        source->key_version, source->key.data(), true);
    if (result != ParticipantCredentialResult::OK) {
      bundle->clear();
      return result == ParticipantCredentialResult::FULL
                 ? CredentialProvisioningResult::REGISTRY_FULL
                 : CredentialProvisioningResult::INVALID_BUNDLE;
    }
  }
  return CredentialProvisioningResult::OK;
}

CredentialProvisioningResult merge_site_credential_bundle(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &bundle,
    ParticipantCredentialRegistry *credentials,
    ParticipantCredentialRegistry *merge_scratch, bool *changed) {
  if (credentials == nullptr || merge_scratch == nullptr ||
      merge_scratch == credentials || changed == nullptr ||
      !configuration.roster_committed ||
      !active_member(configuration, identity.node_id))
    return CredentialProvisioningResult::INVALID_ARGUMENT;
  *changed = false;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        !site_credential_required_for_node(
            identity, configuration, directory, site_leader_node_id,
            member.participant_id))
      continue;
    const auto *required = bundle.find(member.participant_id);
    if (required == nullptr || !required->enabled)
      return CredentialProvisioningResult::INVALID_BUNDLE;
  }
  merge_scratch->clear();
  if (credentials->size() != bundle.size())
    *changed = true;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *source = bundle.record_at(slot);
    if (source == nullptr)
      continue;
    if (!source->enabled ||
        !site_credential_required_for_node(
            identity, configuration, directory, site_leader_node_id,
            source->participant_id))
      return CredentialProvisioningResult::INVALID_BUNDLE;
    const auto *existing = credentials->find(source->participant_id);
    if (existing == nullptr || !same_record(*existing, *source))
      *changed = true;
    const auto result = merge_scratch->set(
        source->participant_id, source->participant_token,
        source->key_version, source->key.data(), true);
    if (result != ParticipantCredentialResult::OK)
      return result == ParticipantCredentialResult::FULL
                 ? CredentialProvisioningResult::REGISTRY_FULL
                 : CredentialProvisioningResult::INVALID_BUNDLE;
  }
  if (*changed)
    *credentials = *merge_scratch;
  return *changed ? CredentialProvisioningResult::OK
                  : CredentialProvisioningResult::ALREADY_COMPLETE;
}

}  // namespace radar_api_server
}  // namespace esphome
