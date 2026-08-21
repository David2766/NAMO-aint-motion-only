#include "platform_operation.h"

#include "radar_api_server.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

static const char *const TAG = "platform_operation";

bool requires_continuity(PlatformParticipantOperationAction action) {
  return action != PlatformParticipantOperationAction::QUARANTINE &&
         action != PlatformParticipantOperationAction::REVOKE &&
         action != PlatformParticipantOperationAction::FORGET &&
         action != PlatformParticipantOperationAction::INVALIDATE_READINESS;
}

bool needs_readiness(PlatformParticipantOperationAction action) {
  return action == PlatformParticipantOperationAction::CONFIGURE ||
         action == PlatformParticipantOperationAction::RECONFIGURE ||
         action == PlatformParticipantOperationAction::ACTIVATE ||
         action == PlatformParticipantOperationAction::FORGET ||
         action == PlatformParticipantOperationAction::INVALIDATE_READINESS;
}

}  // namespace

PlatformParticipantOperationOutcome PlatformParticipantOperation::execute(
    RadarApiServer &server,
    const PlatformParticipantOperationRequest &request) {
  PlatformParticipantOperationOutcome outcome{};
  if (this->workspace_ == nullptr ||
      !server.participant_registry_persistence_ready() ||
      (request.action ==
           PlatformParticipantOperationAction::SET_COMMUNITY_LEADER &&
       !server.site_configuration_persistence_ready()) ||
      (needs_readiness(request.action) &&
       !server.participant_readiness_persistence_ready())) {
    outcome.result = PlatformParticipantOperationResult::UNAVAILABLE;
    ESP_LOGW(TAG, "Participant operation action=%s participant=%s result=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id != nullptr ? request.participant_id : "-",
             platform_participant_operation_result_to_string(outcome.result));
    return outcome;
  }
  auto lease = this->try_acquire();
  if (!lease.acquired()) {
    outcome.result = PlatformParticipantOperationResult::BUSY;
    ESP_LOGW(TAG, "Participant operation action=%s participant=%s result=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id != nullptr ? request.participant_id : "-",
             platform_participant_operation_result_to_string(outcome.result));
    return outcome;
  }

  if (request.action != PlatformParticipantOperationAction::APPROVE &&
      (request.participant_id == nullptr || request.participant_id[0] == '\0')) {
    outcome.result = PlatformParticipantOperationResult::ENROLLMENT_FAILED;
    outcome.enrollment_result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return outcome;
  }

  const bool local_configuration_owner =
      server.site_configuration_ready() &&
      std::strcmp(server.site_configuration_snapshot()
                      .configuration_owner_node_id,
                  server.platform_identity().node_id) == 0;
  if (requires_continuity(request.action) && local_configuration_owner &&
      !server.shared_site_configuration_writable(request.now_ms)) {
    outcome.result =
        PlatformParticipantOperationResult::CONTINUITY_UNAVAILABLE;
    ESP_LOGW(TAG, "Participant operation action=%s participant=%s result=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id != nullptr ? request.participant_id : "-",
             platform_participant_operation_result_to_string(outcome.result));
    return outcome;
  }

  this->workspace_->participant_rollback =
      server.participant_registry_snapshot().registry;
  if (server.participant_readiness_persistence_ready()) {
    this->workspace_->readiness_rollback =
        server.participant_readiness_snapshot().registry;
  }
  const bool credentials_available =
      server.participant_credential_persistence_ready();
  if (credentials_available) {
    this->workspace_->credential_rollback =
        server.participant_credential_snapshot().registry;
  }

  const uint32_t registry_generation_before =
      server.participant_registry_snapshot().generation;
  const uint32_t readiness_generation_before =
      server.participant_readiness_persistence_ready()
          ? server.participant_readiness_snapshot().generation
          : 0;
  const uint32_t credential_generation_before =
      credentials_available
          ? server.participant_credential_snapshot().generation
          : 0;
  auto &enrollment = server.participant_enrollment();
  auto &registry_snapshot = server.mutable_participant_registry_snapshot();
  auto &registry_backend = *server.participant_registry_backend();
  auto &registry_store = *server.participant_registry_store();
  ParticipantEnrollmentResult enrollment_result =
      ParticipantEnrollmentResult::INVALID_ARGUMENT;

  const auto *canonical_member =
      local_configuration_owner && request.participant_id != nullptr
          ? server.site_configuration_snapshot().find(request.participant_id)
          : nullptr;
  if (request.action ==
      PlatformParticipantOperationAction::SET_COMMUNITY_LEADER) {
    auto &configuration_update = server.site_configuration_update_scratch();
    const auto build_result = set_local_site_member_leader_eligible(
        server.platform_identity(), server.site_configuration_snapshot(),
        request.participant_id, request.community_leader_eligible,
        &configuration_update);
    if (build_result != SiteConfigurationResult::OK) {
      outcome.result = PlatformParticipantOperationResult::ENROLLMENT_FAILED;
      outcome.enrollment_result =
          build_result == SiteConfigurationResult::INVALID_ARGUMENT
              ? ParticipantEnrollmentResult::INVALID_ARGUMENT
              : ParticipantEnrollmentResult::INVALID_STATE;
      ESP_LOGW(TAG,
               "Participant operation action=%s participant=%s result=%s "
               "canonical=%u",
               platform_participant_operation_action_to_string(request.action),
               request.participant_id,
               platform_participant_operation_result_to_string(outcome.result),
               static_cast<unsigned>(build_result));
      return outcome;
    }

    const auto *direct_member =
        registry_snapshot.registry.find(request.participant_id);
    const bool direct_pair = direct_member != nullptr;
    if (direct_member != nullptr &&
        direct_member->leader_eligible !=
            request.community_leader_eligible) {
      enrollment_result = enrollment.set_community_leader_eligible(
          registry_backend, registry_store, &registry_snapshot,
          request.participant_id, request.community_leader_eligible);
      if (enrollment_result != ParticipantEnrollmentResult::OK) {
        outcome.result = PlatformParticipantOperationResult::ENROLLMENT_FAILED;
        outcome.enrollment_result = enrollment_result;
        ESP_LOGW(TAG,
                 "Participant operation action=%s participant=%s result=%s "
                 "direct_pair=true enrollment=%s",
                 platform_participant_operation_action_to_string(
                     request.action),
                 request.participant_id,
                 platform_participant_operation_result_to_string(
                     outcome.result),
                 participant_enrollment_result_to_string(enrollment_result));
        return outcome;
      }
    }

    const bool registry_changed =
        registry_snapshot.generation != registry_generation_before;
    const bool site_unchanged =
        configuration_update.roster_revision ==
        server.site_configuration_snapshot().roster_revision;
    const bool site_updated =
        site_unchanged ||
        server.site_configuration_store()->update(
            *server.site_configuration_backend(), configuration_update,
            &server.mutable_site_configuration_snapshot()) ==
            SiteConfigurationStoreResult::UPDATED;
    if (!site_updated) {
      outcome.rollback_attempted = registry_changed;
      outcome.rollback_succeeded =
          !registry_changed ||
          this->rollback_(server, true, false, false);
      outcome.result =
          outcome.rollback_succeeded
              ? PlatformParticipantOperationResult::ROSTER_PERSISTENCE_FAILED
              : PlatformParticipantOperationResult::ROLLBACK_FAILED;
      outcome.enrollment_result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
      ESP_LOGE(TAG,
               "Participant operation action=%s participant=%s result=%s "
               "direct_pair=%s rollback=%s",
               platform_participant_operation_action_to_string(request.action),
               request.participant_id,
               platform_participant_operation_result_to_string(outcome.result),
               direct_pair ? "true" : "false",
               outcome.rollback_attempted
                   ? (outcome.rollback_succeeded ? "restored" : "failed")
                   : "not_required");
      return outcome;
    }

    outcome.result = PlatformParticipantOperationResult::OK;
    outcome.enrollment_result = ParticipantEnrollmentResult::OK;
    ESP_LOGI(TAG,
             "Participant operation action=%s participant=%s result=ok "
             "direct_pair=%s eligible=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id,
             direct_pair ? "true" : "false",
             request.community_leader_eligible ? "true" : "false");
    return outcome;
  }
  const bool canonical_only_management =
      canonical_member != nullptr &&
      registry_snapshot.registry.find(request.participant_id) == nullptr &&
      (request.action == PlatformParticipantOperationAction::REVOKE ||
       request.action == PlatformParticipantOperationAction::FORGET);
  if (canonical_only_management) {
    bool readiness_updated = false;
    if (request.action == PlatformParticipantOperationAction::FORGET) {
      auto &readiness_snapshot =
          server.mutable_participant_readiness_snapshot();
      if (readiness_snapshot.registry.find(request.participant_id) != nullptr) {
        readiness_snapshot.registry.forget(request.participant_id);
        readiness_updated =
            server.participant_readiness_store()->update(
                *server.participant_readiness_backend(),
                readiness_snapshot.registry, &readiness_snapshot) ==
            ParticipantReadinessStoreResult::UPDATED;
        if (!readiness_updated) {
          server.participant_readiness_store()->update(
              *server.participant_readiness_backend(),
              this->workspace_->readiness_rollback, &readiness_snapshot);
          outcome.result =
              PlatformParticipantOperationResult::ENROLLMENT_FAILED;
          outcome.enrollment_result =
              ParticipantEnrollmentResult::PERSISTENCE_FAILED;
          return outcome;
        }
      }
    }

    const bool credential_updated =
        !credentials_available ||
        (request.action == PlatformParticipantOperationAction::REVOKE
             ? server.disable_participant_credential(request.participant_id)
             : server.erase_participant_credential(request.participant_id));
    if (!credential_updated) {
      outcome.rollback_attempted = readiness_updated;
      outcome.rollback_succeeded =
          !readiness_updated ||
          server.participant_readiness_store()->update(
              *server.participant_readiness_backend(),
              this->workspace_->readiness_rollback,
              &server.mutable_participant_readiness_snapshot()) ==
              ParticipantReadinessStoreResult::UPDATED;
      outcome.result = outcome.rollback_succeeded
                           ? PlatformParticipantOperationResult::
                                 ENROLLMENT_FAILED
                           : PlatformParticipantOperationResult::ROLLBACK_FAILED;
      outcome.enrollment_result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
      return outcome;
    }

    auto &configuration_update = server.site_configuration_update_scratch();
    const auto build_result =
        request.action == PlatformParticipantOperationAction::REVOKE
            ? revoke_local_site_member(
                  server.platform_identity(),
                  server.site_configuration_snapshot(), request.participant_id,
                  &configuration_update)
            : forget_local_site_member(
                  server.platform_identity(),
                  server.site_configuration_snapshot(), request.participant_id,
                  &configuration_update);
    const bool site_unchanged =
        build_result == SiteConfigurationResult::OK &&
        configuration_update.roster_revision ==
            server.site_configuration_snapshot().roster_revision;
    const bool site_updated =
        site_unchanged ||
        (build_result == SiteConfigurationResult::OK &&
         server.site_configuration_store()->update(
             *server.site_configuration_backend(), configuration_update,
             &server.mutable_site_configuration_snapshot()) ==
             SiteConfigurationStoreResult::UPDATED);
    if (!site_updated) {
      outcome.rollback_attempted = readiness_updated || credentials_available;
      const bool readiness_restored =
          !readiness_updated ||
          server.participant_readiness_store()->update(
              *server.participant_readiness_backend(),
              this->workspace_->readiness_rollback,
              &server.mutable_participant_readiness_snapshot()) ==
              ParticipantReadinessStoreResult::UPDATED;
      const bool credentials_restored =
          !credentials_available || server.restore_participant_credentials(
                                        this->workspace_->credential_rollback);
      outcome.rollback_succeeded =
          readiness_restored && credentials_restored;
      outcome.result = outcome.rollback_succeeded
                           ? PlatformParticipantOperationResult::
                                 ROSTER_PERSISTENCE_FAILED
                           : PlatformParticipantOperationResult::ROLLBACK_FAILED;
      outcome.enrollment_result =
          build_result == SiteConfigurationResult::OK
              ? ParticipantEnrollmentResult::PERSISTENCE_FAILED
              : ParticipantEnrollmentResult::INVALID_STATE;
      return outcome;
    }

    outcome.result = PlatformParticipantOperationResult::OK;
    outcome.enrollment_result = ParticipantEnrollmentResult::OK;
    ESP_LOGI(TAG,
             "Canonical participant operation action=%s participant=%s "
             "result=ok direct_pair=false",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id);
    return outcome;
  }

  switch (request.action) {
    case PlatformParticipantOperationAction::APPROVE: {
      enrollment_result = enrollment.approve_candidate(
          registry_backend, registry_store, &registry_snapshot,
          request.approval_trust, request.now_ms);
      break;
    }
    case PlatformParticipantOperationAction::CONFIGURE:
    case PlatformParticipantOperationAction::INVALIDATE_READINESS: {
      if (request.participant_id == nullptr ||
          request.configuration == nullptr) {
        enrollment_result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
        break;
      }
      enrollment_result = enrollment.configure_participant(
          registry_backend, registry_store, &registry_snapshot,
          *server.participant_readiness_backend(),
          *server.participant_readiness_store(),
          &server.mutable_participant_readiness_snapshot(),
          request.participant_id, *request.configuration);
      break;
    }
    case PlatformParticipantOperationAction::RECONFIGURE:
      if (request.participant_id == nullptr ||
          request.configuration == nullptr) {
        enrollment_result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
        break;
      }
      enrollment_result = enrollment.reconfigure_active_participant(
          server.participant_registry_snapshot(),
          *server.participant_readiness_backend(),
          *server.participant_readiness_store(),
          &server.mutable_participant_readiness_snapshot(),
          request.participant_id, *request.configuration);
      break;
    case PlatformParticipantOperationAction::ACTIVATE: {
      if (request.participant_id == nullptr) {
        enrollment_result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
        break;
      }
      const auto *credential =
          credentials_available
              ? server.participant_credential_snapshot().registry.find(
                    request.participant_id)
              : nullptr;
      if (credential != nullptr && !credential->enabled) {
        if (!server.enable_participant_credential(request.participant_id)) {
          enrollment_result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
          break;
        }
      }
      enrollment_result = enrollment.activate(
          registry_backend, registry_store, &registry_snapshot,
          server.participant_readiness_snapshot(), request.participant_id);
      break;
    }
    case PlatformParticipantOperationAction::QUARANTINE:
      enrollment_result = enrollment.quarantine(
          registry_backend, registry_store, &registry_snapshot,
          request.participant_id);
      break;
    case PlatformParticipantOperationAction::REVOKE:
      enrollment_result = enrollment.revoke(
          registry_backend, registry_store, &registry_snapshot,
          request.participant_id);
      if (enrollment_result == ParticipantEnrollmentResult::OK &&
          credentials_available) {
        if (!server.disable_participant_credential(request.participant_id))
          enrollment_result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
      }
      break;
    case PlatformParticipantOperationAction::FORGET:
      enrollment_result = enrollment.forget(
          registry_backend, registry_store, &registry_snapshot,
          *server.participant_readiness_backend(),
          *server.participant_readiness_store(),
          &server.mutable_participant_readiness_snapshot(),
          request.participant_id);
      if (enrollment_result == ParticipantEnrollmentResult::OK &&
          credentials_available) {
        if (!server.erase_participant_credential(request.participant_id))
          enrollment_result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
      }
      break;
    case PlatformParticipantOperationAction::SET_COMMUNITY_LEADER:
      enrollment_result = enrollment.set_community_leader_eligible(
          registry_backend, registry_store, &registry_snapshot,
          request.participant_id, request.community_leader_eligible);
      break;
  }

  const bool registry_changed =
      server.participant_registry_snapshot().generation !=
      registry_generation_before;
  const bool readiness_changed =
      server.participant_readiness_persistence_ready() &&
      server.participant_readiness_snapshot().generation !=
          readiness_generation_before;
  const bool credential_changed =
      credentials_available &&
      server.participant_credential_snapshot().generation !=
          credential_generation_before;

  outcome.enrollment_result = enrollment_result;
  if (enrollment_result != ParticipantEnrollmentResult::OK) {
    outcome.rollback_attempted =
        registry_changed || readiness_changed || credential_changed;
    outcome.rollback_succeeded =
        !outcome.rollback_attempted ||
        this->rollback_(server, registry_changed, readiness_changed,
                        credential_changed);
    outcome.result = outcome.rollback_succeeded
                         ? PlatformParticipantOperationResult::ENROLLMENT_FAILED
                         : PlatformParticipantOperationResult::ROLLBACK_FAILED;
    ESP_LOGW(TAG,
             "Participant operation action=%s participant=%s result=%s "
             "enrollment=%s rollback=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id != nullptr ? request.participant_id : "-",
             platform_participant_operation_result_to_string(outcome.result),
             participant_enrollment_result_to_string(enrollment_result),
             outcome.rollback_attempted
                 ? (outcome.rollback_succeeded ? "restored" : "failed")
                 : "not_required");
    return outcome;
  }

  const bool staged_canonical_configure =
      request.action == PlatformParticipantOperationAction::CONFIGURE &&
      local_configuration_owner && canonical_member != nullptr &&
      canonical_member->membership == ParticipantMembership::QUARANTINED;
  if (staged_canonical_configure) {
    outcome.result = PlatformParticipantOperationResult::OK;
    outcome.enrollment_result = ParticipantEnrollmentResult::OK;
    ESP_LOGI(TAG,
             "Participant operation action=configure participant=%s result=ok "
             "canonical=staged",
             request.participant_id);
    return outcome;
  }

  const bool activation_commit =
      request.action == PlatformParticipantOperationAction::ACTIVATE;
  const char *spatial_profile_participant_id =
      activation_commit || request.automatic_group ? request.participant_id
                                                   : nullptr;
  if (!server.refresh_owned_site_roster(spatial_profile_participant_id,
                                        request.automatic_group)) {
    outcome.rollback_attempted = true;
    outcome.rollback_succeeded = this->rollback_(
        server, registry_changed, readiness_changed, credential_changed);
    outcome.result =
        outcome.rollback_succeeded
            ? PlatformParticipantOperationResult::ROSTER_PERSISTENCE_FAILED
            : PlatformParticipantOperationResult::ROLLBACK_FAILED;
    ESP_LOGE(TAG,
             "Participant operation action=%s participant=%s result=%s "
             "rollback=%s",
             platform_participant_operation_action_to_string(request.action),
             request.participant_id != nullptr ? request.participant_id : "-",
             platform_participant_operation_result_to_string(outcome.result),
             outcome.rollback_succeeded ? "restored" : "failed");
    return outcome;
  }

  outcome.result = PlatformParticipantOperationResult::OK;
  outcome.enrollment_result = ParticipantEnrollmentResult::OK;
  ESP_LOGI(TAG, "Participant operation action=%s participant=%s result=ok",
           platform_participant_operation_action_to_string(request.action),
           request.participant_id != nullptr ? request.participant_id : "-");
  return outcome;
}

bool PlatformParticipantOperation::rollback_(RadarApiServer &server,
                                             bool registry_changed,
                                             bool readiness_changed,
                                             bool credential_changed) {
  bool prerequisites_restored = true;
  if (readiness_changed &&
      server.participant_readiness_persistence_ready()) {
    prerequisites_restored =
        server.participant_readiness_store()->update(
            *server.participant_readiness_backend(),
            this->workspace_->readiness_rollback,
            &server.mutable_participant_readiness_snapshot()) ==
        ParticipantReadinessStoreResult::UPDATED;
  }
  if (credential_changed &&
      server.participant_credential_persistence_ready()) {
    prerequisites_restored =
        server.restore_participant_credentials(
            this->workspace_->credential_rollback) &&
        prerequisites_restored;
  }
  if (!prerequisites_restored)
    return false;
  if (!registry_changed)
    return true;
  return server.participant_registry_store()->update(
             *server.participant_registry_backend(),
             this->workspace_->participant_rollback,
             &server.mutable_participant_registry_snapshot()) ==
         ParticipantRegistryStoreResult::UPDATED;
}

const char *platform_participant_operation_action_to_string(
    PlatformParticipantOperationAction action) {
  switch (action) {
    case PlatformParticipantOperationAction::APPROVE:
      return "approve";
    case PlatformParticipantOperationAction::CONFIGURE:
      return "configure";
    case PlatformParticipantOperationAction::RECONFIGURE:
      return "reconfigure";
    case PlatformParticipantOperationAction::ACTIVATE:
      return "activate";
    case PlatformParticipantOperationAction::QUARANTINE:
      return "quarantine";
    case PlatformParticipantOperationAction::REVOKE:
      return "revoke";
    case PlatformParticipantOperationAction::FORGET:
      return "forget";
    case PlatformParticipantOperationAction::SET_COMMUNITY_LEADER:
      return "set_community_leader";
    case PlatformParticipantOperationAction::INVALIDATE_READINESS:
      return "invalidate_readiness";
  }
  return "unknown";
}

const char *platform_participant_operation_result_to_string(
    PlatformParticipantOperationResult result) {
  switch (result) {
    case PlatformParticipantOperationResult::OK:
      return "ok";
    case PlatformParticipantOperationResult::BUSY:
      return "busy";
    case PlatformParticipantOperationResult::UNAVAILABLE:
      return "unavailable";
    case PlatformParticipantOperationResult::CONTINUITY_UNAVAILABLE:
      return "site_configuration_continuity_unavailable";
    case PlatformParticipantOperationResult::ENROLLMENT_FAILED:
      return "enrollment_failed";
    case PlatformParticipantOperationResult::ROSTER_PERSISTENCE_FAILED:
      return "roster_persistence_failed";
    case PlatformParticipantOperationResult::ROLLBACK_FAILED:
      return "rollback_failed";
  }
  return "unknown";
}

}  // namespace radar_api_server
}  // namespace esphome
