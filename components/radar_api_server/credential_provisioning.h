#pragma once

#include "participant_credentials.h"
#include "platform_identity.h"
#include "group_roster.h"
#include "site_configuration.h"

#include <cstdint>

namespace esphome {
namespace radar_api_server {

struct SiteDirectorySnapshot;
struct ParticipantEndpoint;
class ParticipantEndpointCache;

enum class CredentialProvisioningResult : uint8_t {
  OK = 0,
  ALREADY_COMPLETE,
  INVALID_ARGUMENT,
  NOT_OWNER,
  REQUESTER_INACTIVE,
  SOURCE_CREDENTIAL_MISSING,
  INVALID_BUNDLE,
  REGISTRY_FULL,
};

enum class CredentialProvisioningContextChange : uint8_t {
  NONE = 0,
  ROSTER_REFRESH,
  AUTHORITY_REFRESH,
};

enum class CredentialProvisioningSourceReadiness : uint8_t {
  READY = 0,
  ENDPOINT_MISSING,
  ENDPOINT_UNAUTHENTICATED,
  ENDPOINT_ADDRESS_MISSING,
  ENDPOINT_OFFLINE,
  LEGACY_HEARTBEAT,
  AUTHORITY_EPOCH_MISMATCH,
  CONFIGURATION_GENERATION_MISMATCH,
  CONFIGURATION_DIGEST_MISMATCH,
  TOPOLOGY_REVISION_MISMATCH,
  TOPOLOGY_DIGEST_MISMATCH,
};

struct CredentialReconciliationSummary {
  uint8_t preserved_canonical{0};
  uint8_t erased_orphans{0};
  uint8_t disabled_revoked{0};

  bool changed() const {
    return erased_orphans != 0 || disabled_revoked != 0;
  }
};

CredentialProvisioningContextChange credential_provisioning_context_change(
    uint32_t previous_authority_epoch, uint32_t previous_roster_revision,
    const char *previous_owner_node_id,
    const SiteConfigurationSnapshot &configuration);

const char *credential_provisioning_result_to_string(
    CredentialProvisioningResult result);
const char *credential_provisioning_source_readiness_to_string(
    CredentialProvisioningSourceReadiness readiness);

CredentialProvisioningSourceReadiness
credential_provisioning_source_readiness(
    const ParticipantEndpoint *owner_endpoint,
    const SiteConfigurationSnapshot &configuration, uint32_t now_ms);

bool credential_provisioning_authority_bootstrap_source_ready(
    const ParticipantEndpoint *source_endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *source_participant_id);

CredentialReconciliationSummary reconcile_site_credential_registry(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    ParticipantCredentialRegistry *credentials);

uint32_t credential_provisioning_source_bit(
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id);

const ParticipantEndpoint *select_credential_provisioning_source(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials,
    const char *preferred_fallback_node_id,
    uint32_t attempted_source_mask, uint32_t now_ms,
    bool allow_authority_bootstrap = false);

bool site_credential_required_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const char *candidate_participant_id);

bool site_credentials_complete_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &credentials);

bool site_credential_bootstrap_ready_for_node(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials);

bool group_leader_credentials_complete(
    const char *leader_node_id, const char *group_id,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantCredentialRegistry &credentials);
bool group_leader_credentials_complete(
    const char *leader_node_id, const GroupRosterSnapshot &roster,
    const ParticipantCredentialRegistry &credentials);

const ParticipantCredentialRecord *select_site_sender_credential(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantCredentialRegistry &credentials,
    const char *target_participant_id);

const ParticipantCredentialRecord *credential_probe_at(
    const ParticipantCredentialRegistry &credentials, std::size_t index);

bool credential_bundle_binds_requester(
    const ParticipantCredentialRegistry &bundle,
    const char *configuration_owner_node_id,
    const ParticipantCredentialRecord &request_credential);

CredentialProvisioningResult prepare_site_credential_bundle(
    const PlatformIdentity &owner_identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &owner_credentials,
    const char *requester_participant_id,
    ParticipantCredentialRegistry *bundle);

CredentialProvisioningResult merge_site_credential_bundle(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const char *site_leader_node_id,
    const ParticipantCredentialRegistry &bundle,
    ParticipantCredentialRegistry *credentials,
    ParticipantCredentialRegistry *merge_scratch, bool *changed);

}  // namespace radar_api_server
}  // namespace esphome
