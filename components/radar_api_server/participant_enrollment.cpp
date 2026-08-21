#include "participant_enrollment.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

ParticipantEnrollmentResult ParticipantEnrollment::open_window(uint32_t now_ms,
                                                                 uint32_t duration_ms) {
  if (duration_ms == 0 || duration_ms > MAX_PAIRING_WINDOW_MS)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  this->window_opened_at_ms_ = now_ms;
  this->window_duration_ms_ = duration_ms;
  this->window_open_ = true;
  this->clear_candidate_();
  return ParticipantEnrollmentResult::OK;
}

void ParticipantEnrollment::close_window() {
  this->window_open_ = false;
  this->window_opened_at_ms_ = 0;
  this->window_duration_ms_ = 0;
  this->clear_candidate_();
}

bool ParticipantEnrollment::window_open(uint32_t now_ms) const {
  return this->window_open_ &&
         static_cast<uint32_t>(now_ms - this->window_opened_at_ms_) <
             this->window_duration_ms_;
}

uint32_t ParticipantEnrollment::window_remaining_ms(uint32_t now_ms) const {
  if (!this->window_open(now_ms))
    return 0;
  return this->window_duration_ms_ -
         static_cast<uint32_t>(now_ms - this->window_opened_at_ms_);
}

bool ParticipantEnrollment::has_pending_candidate(uint32_t now_ms) const {
  return this->window_open(now_ms) && this->candidate_.occupied;
}

const ParticipantRecord *ParticipantEnrollment::pending_candidate(uint32_t now_ms) const {
  return this->has_pending_candidate(now_ms) ? &this->candidate_ : nullptr;
}

ParticipantEnrollmentResult ParticipantEnrollment::observe_candidate(
    const ParticipantRegistrySnapshot &snapshot, const char *participant_id,
    ParticipantType type, uint32_t now_ms) {
  if (this->expire_window_(now_ms))
    return ParticipantEnrollmentResult::WINDOW_EXPIRED;
  if (!this->window_open_)
    return ParticipantEnrollmentResult::WINDOW_CLOSED;
  if (!valid_type_(type) || participant_id == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (snapshot.generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  if (this->candidate_.occupied) {
    if (this->candidate_.type == type &&
        std::strcmp(this->candidate_.participant_id, participant_id) == 0)
      return ParticipantEnrollmentResult::OK;
    return ParticipantEnrollmentResult::CANDIDATE_PENDING;
  }

  this->working_registry_ = snapshot.registry;
  const auto discover_result = this->working_registry_.discover(participant_id, type);
  if (discover_result != ParticipantRegistryResult::OK)
    return registry_result_(discover_result);

  const ParticipantRecord *candidate = this->working_registry_.find(participant_id);
  if (candidate == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  this->candidate_ = *candidate;
  return ParticipantEnrollmentResult::OK;
}

ParticipantEnrollmentResult ParticipantEnrollment::approve_candidate(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot, ParticipantTrust trust, uint32_t now_ms) {
  if (snapshot == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (this->expire_window_(now_ms))
    return ParticipantEnrollmentResult::WINDOW_EXPIRED;
  if (!this->window_open_)
    return ParticipantEnrollmentResult::WINDOW_CLOSED;
  if (!this->candidate_.occupied)
    return ParticipantEnrollmentResult::NO_CANDIDATE;
  if (snapshot->generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  // No caller may mint official trust until the attestation verifier exists.
  if (trust == ParticipantTrust::OFFICIAL_VERIFIED)
    return ParticipantEnrollmentResult::ATTESTATION_REQUIRED;
  if ((this->candidate_.type == ParticipantType::NAMO_NODE &&
       trust != ParticipantTrust::COMMUNITY_UNVERIFIED) ||
      (this->candidate_.type == ParticipantType::SATELLITE &&
       trust != ParticipantTrust::SATELLITE))
    return ParticipantEnrollmentResult::INCOMPATIBLE_TRUST;

  this->working_registry_ = snapshot->registry;
  auto registry_result = this->working_registry_.discover(
      this->candidate_.participant_id, this->candidate_.type);
  if (registry_result != ParticipantRegistryResult::OK)
    return registry_result_(registry_result);
  registry_result = this->working_registry_.classify(this->candidate_.participant_id, trust);
  if (registry_result != ParticipantRegistryResult::OK)
    return registry_result_(registry_result);

  const auto persist_result = this->persist_working_(backend, store, snapshot);
  if (persist_result == ParticipantEnrollmentResult::OK)
    this->close_window();
  return persist_result;
}

ParticipantEnrollmentResult ParticipantEnrollment::activate(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot,
    const ParticipantReadinessSnapshot &readiness_snapshot,
    const char *participant_id) {
  if (participant_id == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (readiness_snapshot.generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;
  if (!ParticipantReadinessRegistry::ready(
          readiness_snapshot.registry.find(participant_id)))
    return ParticipantEnrollmentResult::NOT_READY;
  return this->mutate_(backend, store, snapshot, participant_id, Mutation::ACTIVATE);
}

ParticipantEnrollmentResult ParticipantEnrollment::configure_participant(
    ParticipantRegistryStorageBackend &registry_backend,
    ParticipantRegistryStore &registry_store,
    ParticipantRegistrySnapshot *registry_snapshot,
    ParticipantReadinessStorageBackend &readiness_backend,
    ParticipantReadinessStore &readiness_store,
    ParticipantReadinessSnapshot *readiness_snapshot,
    const char *participant_id, const ParticipantReadinessConfig &config) {
  if (registry_snapshot == nullptr || readiness_snapshot == nullptr ||
      participant_id == nullptr ||
      !ParticipantReadinessRegistry::valid_config(config))
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (registry_snapshot->generation == 0 || readiness_snapshot->generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  const ParticipantRecord *participant =
      registry_snapshot->registry.find(participant_id);
  if (participant == nullptr)
    return ParticipantEnrollmentResult::NOT_FOUND;
  if (participant->membership == ParticipantMembership::REVOKED ||
      participant->trust == ParticipantTrust::UNKNOWN)
    return ParticipantEnrollmentResult::INVALID_STATE;

  // Configuration changes cannot race with active observation admission.
  // Persist quarantine first, then update the profile. A later failure is safe:
  // the participant remains trusted but inactive.
  if (participant->membership == ParticipantMembership::ACTIVE) {
    const auto quarantine_result = this->quarantine(
        registry_backend, registry_store, registry_snapshot, participant_id);
    if (quarantine_result != ParticipantEnrollmentResult::OK)
      return quarantine_result;
  }

  const ParticipantReadinessRecord *existing =
      readiness_snapshot->registry.find(participant_id);
  const bool had_existing = existing != nullptr;
  ParticipantReadinessRecord backup{};
  if (had_existing)
    backup = *existing;

  if (readiness_snapshot->registry.configure(participant_id, config) !=
      ParticipantReadinessResult::OK)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  const auto persist_result = readiness_store.update(
      readiness_backend, readiness_snapshot->registry, readiness_snapshot);
  if (persist_result == ParticipantReadinessStoreResult::UPDATED)
    return ParticipantEnrollmentResult::OK;

  if (had_existing) {
    readiness_snapshot->registry.configure(participant_id, backup.config);
  } else {
    readiness_snapshot->registry.forget(participant_id);
  }
  return ParticipantEnrollmentResult::PERSISTENCE_FAILED;
}

ParticipantEnrollmentResult
ParticipantEnrollment::reconfigure_active_participant(
    const ParticipantRegistrySnapshot &registry_snapshot,
    ParticipantReadinessStorageBackend &readiness_backend,
    ParticipantReadinessStore &readiness_store,
    ParticipantReadinessSnapshot *readiness_snapshot,
    const char *participant_id, const ParticipantReadinessConfig &config) {
  if (readiness_snapshot == nullptr || participant_id == nullptr ||
      !ParticipantReadinessRegistry::valid_config(config))
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (registry_snapshot.generation == 0 || readiness_snapshot->generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  const ParticipantRecord *participant =
      registry_snapshot.registry.find(participant_id);
  const ParticipantReadinessRecord *existing =
      readiness_snapshot->registry.find(participant_id);
  if (participant == nullptr || existing == nullptr)
    return ParticipantEnrollmentResult::NOT_FOUND;
  if (participant->membership != ParticipantMembership::ACTIVE ||
      participant->trust == ParticipantTrust::UNKNOWN)
    return ParticipantEnrollmentResult::INVALID_STATE;

  const ParticipantReadinessRecord backup = *existing;
  if (readiness_snapshot->registry.configure(participant_id, config) !=
      ParticipantReadinessResult::OK)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  const auto persist_result = readiness_store.update(
      readiness_backend, readiness_snapshot->registry, readiness_snapshot);
  if (persist_result == ParticipantReadinessStoreResult::UPDATED)
    return ParticipantEnrollmentResult::OK;

  readiness_snapshot->registry.configure(participant_id, backup.config);
  return ParticipantEnrollmentResult::PERSISTENCE_FAILED;
}

ParticipantEnrollmentResult ParticipantEnrollment::set_community_leader_eligible(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot, const char *participant_id, bool eligible) {
  return this->mutate_(backend, store, snapshot, participant_id,
                       Mutation::SET_COMMUNITY_LEADER, eligible);
}

ParticipantEnrollmentResult ParticipantEnrollment::quarantine(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot, const char *participant_id) {
  return this->mutate_(backend, store, snapshot, participant_id, Mutation::QUARANTINE);
}

ParticipantEnrollmentResult ParticipantEnrollment::revoke(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot, const char *participant_id) {
  return this->mutate_(backend, store, snapshot, participant_id, Mutation::REVOKE);
}

ParticipantEnrollmentResult ParticipantEnrollment::forget(
    ParticipantRegistryStorageBackend &registry_backend,
    ParticipantRegistryStore &registry_store,
    ParticipantRegistrySnapshot *registry_snapshot,
    ParticipantReadinessStorageBackend &readiness_backend,
    ParticipantReadinessStore &readiness_store,
    ParticipantReadinessSnapshot *readiness_snapshot,
    const char *participant_id) {
  if (registry_snapshot == nullptr || readiness_snapshot == nullptr ||
      participant_id == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (registry_snapshot->generation == 0 || readiness_snapshot->generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  const ParticipantRecord *participant =
      registry_snapshot->registry.find(participant_id);
  if (participant == nullptr)
    return ParticipantEnrollmentResult::NOT_FOUND;
  if (participant->membership == ParticipantMembership::ACTIVE)
    return ParticipantEnrollmentResult::INVALID_STATE;

  const ParticipantReadinessRecord *existing =
      readiness_snapshot->registry.find(participant_id);
  if (existing != nullptr) {
    const ParticipantReadinessRecord backup = *existing;
    readiness_snapshot->registry.forget(participant_id);
    const auto readiness_result = readiness_store.update(
        readiness_backend, readiness_snapshot->registry, readiness_snapshot);
    if (readiness_result != ParticipantReadinessStoreResult::UPDATED) {
      readiness_snapshot->registry.configure(participant_id, backup.config);
      return ParticipantEnrollmentResult::PERSISTENCE_FAILED;
    }
  }
  return this->mutate_(registry_backend, registry_store, registry_snapshot,
                       participant_id, Mutation::FORGET);
}

bool ParticipantEnrollment::expire_window_(uint32_t now_ms) {
  if (!this->window_open_ || this->window_open(now_ms))
    return false;
  this->close_window();
  return true;
}

void ParticipantEnrollment::clear_candidate_() { this->candidate_ = ParticipantRecord{}; }

ParticipantEnrollmentResult ParticipantEnrollment::persist_working_(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot) {
  const auto result = store.update(backend, this->working_registry_, snapshot);
  return result == ParticipantRegistryStoreResult::UPDATED
             ? ParticipantEnrollmentResult::OK
             : ParticipantEnrollmentResult::PERSISTENCE_FAILED;
}

ParticipantEnrollmentResult ParticipantEnrollment::mutate_(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
    ParticipantRegistrySnapshot *snapshot, const char *participant_id,
    Mutation mutation, bool value) {
  if (snapshot == nullptr || participant_id == nullptr)
    return ParticipantEnrollmentResult::INVALID_ARGUMENT;
  if (snapshot->generation == 0)
    return ParticipantEnrollmentResult::PERSISTENCE_FAILED;

  this->working_registry_ = snapshot->registry;
  ParticipantRegistryResult result = ParticipantRegistryResult::INVALID_STATE;
  switch (mutation) {
    case Mutation::ACTIVATE:
      result = this->working_registry_.activate(participant_id);
      break;
    case Mutation::SET_COMMUNITY_LEADER:
      result = this->working_registry_.set_community_leader_eligible(participant_id, value);
      break;
    case Mutation::QUARANTINE:
      result = this->working_registry_.quarantine(participant_id);
      break;
    case Mutation::REVOKE:
      result = this->working_registry_.revoke(participant_id);
      break;
    case Mutation::FORGET:
      result = this->working_registry_.forget(participant_id);
      break;
  }
  if (result != ParticipantRegistryResult::OK)
    return registry_result_(result);
  return this->persist_working_(backend, store, snapshot);
}

ParticipantEnrollmentResult ParticipantEnrollment::registry_result_(
    ParticipantRegistryResult result) {
  switch (result) {
    case ParticipantRegistryResult::OK:
      return ParticipantEnrollmentResult::OK;
    case ParticipantRegistryResult::INVALID_ID:
      return ParticipantEnrollmentResult::INVALID_ARGUMENT;
    case ParticipantRegistryResult::ALREADY_EXISTS:
      return ParticipantEnrollmentResult::ALREADY_REGISTERED;
    case ParticipantRegistryResult::NOT_FOUND:
      return ParticipantEnrollmentResult::NOT_FOUND;
    case ParticipantRegistryResult::FULL:
      return ParticipantEnrollmentResult::REGISTRY_FULL;
    case ParticipantRegistryResult::INCOMPATIBLE_TRUST:
      return ParticipantEnrollmentResult::INCOMPATIBLE_TRUST;
    case ParticipantRegistryResult::INVALID_STATE:
      return ParticipantEnrollmentResult::INVALID_STATE;
  }
  return ParticipantEnrollmentResult::INVALID_STATE;
}

bool ParticipantEnrollment::valid_type_(ParticipantType type) {
  return type == ParticipantType::NAMO_NODE || type == ParticipantType::SATELLITE;
}

const char *participant_enrollment_result_to_string(ParticipantEnrollmentResult result) {
  switch (result) {
    case ParticipantEnrollmentResult::OK:
      return "ok";
    case ParticipantEnrollmentResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case ParticipantEnrollmentResult::WINDOW_CLOSED:
      return "window_closed";
    case ParticipantEnrollmentResult::WINDOW_EXPIRED:
      return "window_expired";
    case ParticipantEnrollmentResult::CANDIDATE_PENDING:
      return "candidate_pending";
    case ParticipantEnrollmentResult::NO_CANDIDATE:
      return "no_candidate";
    case ParticipantEnrollmentResult::ALREADY_REGISTERED:
      return "already_registered";
    case ParticipantEnrollmentResult::REGISTRY_FULL:
      return "registry_full";
    case ParticipantEnrollmentResult::INCOMPATIBLE_TRUST:
      return "incompatible_trust";
    case ParticipantEnrollmentResult::ATTESTATION_REQUIRED:
      return "attestation_required";
    case ParticipantEnrollmentResult::NOT_FOUND:
      return "not_found";
    case ParticipantEnrollmentResult::INVALID_STATE:
      return "invalid_state";
    case ParticipantEnrollmentResult::NOT_READY:
      return "not_ready";
    case ParticipantEnrollmentResult::PERSISTENCE_FAILED:
      return "persistence_failed";
  }
  return "unknown";
}

}  // namespace radar_api_server
}  // namespace esphome
