#pragma once

#include "participant_readiness.h"
#include "participant_registry_store.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

enum class ParticipantEnrollmentResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  WINDOW_CLOSED,
  WINDOW_EXPIRED,
  CANDIDATE_PENDING,
  NO_CANDIDATE,
  ALREADY_REGISTERED,
  REGISTRY_FULL,
  INCOMPATIBLE_TRUST,
  ATTESTATION_REQUIRED,
  NOT_FOUND,
  INVALID_STATE,
  NOT_READY,
  PERSISTENCE_FAILED,
};

const char *participant_enrollment_result_to_string(ParticipantEnrollmentResult result);

class ParticipantEnrollment {
 public:
  static constexpr uint32_t MAX_PAIRING_WINDOW_MS = 5U * 60U * 1000U;

  ParticipantEnrollmentResult open_window(uint32_t now_ms, uint32_t duration_ms);
  void close_window();
  bool window_open(uint32_t now_ms) const;
  uint32_t window_remaining_ms(uint32_t now_ms) const;
  bool has_pending_candidate(uint32_t now_ms) const;
  const ParticipantRecord *pending_candidate(uint32_t now_ms) const;

  ParticipantEnrollmentResult observe_candidate(
      const ParticipantRegistrySnapshot &snapshot, const char *participant_id,
      ParticipantType type, uint32_t now_ms);
  ParticipantEnrollmentResult approve_candidate(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot, ParticipantTrust trust, uint32_t now_ms);

  ParticipantEnrollmentResult activate(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot,
      const ParticipantReadinessSnapshot &readiness_snapshot,
      const char *participant_id);
  ParticipantEnrollmentResult configure_participant(
      ParticipantRegistryStorageBackend &registry_backend,
      ParticipantRegistryStore &registry_store,
      ParticipantRegistrySnapshot *registry_snapshot,
      ParticipantReadinessStorageBackend &readiness_backend,
      ParticipantReadinessStore &readiness_store,
      ParticipantReadinessSnapshot *readiness_snapshot,
      const char *participant_id, const ParticipantReadinessConfig &config);
  ParticipantEnrollmentResult reconfigure_active_participant(
      const ParticipantRegistrySnapshot &registry_snapshot,
      ParticipantReadinessStorageBackend &readiness_backend,
      ParticipantReadinessStore &readiness_store,
      ParticipantReadinessSnapshot *readiness_snapshot,
      const char *participant_id, const ParticipantReadinessConfig &config);
  ParticipantEnrollmentResult set_community_leader_eligible(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot, const char *participant_id, bool eligible);
  ParticipantEnrollmentResult quarantine(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot, const char *participant_id);
  ParticipantEnrollmentResult revoke(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot, const char *participant_id);
  ParticipantEnrollmentResult forget(
      ParticipantRegistryStorageBackend &registry_backend,
      ParticipantRegistryStore &registry_store,
      ParticipantRegistrySnapshot *registry_snapshot,
      ParticipantReadinessStorageBackend &readiness_backend,
      ParticipantReadinessStore &readiness_store,
      ParticipantReadinessSnapshot *readiness_snapshot,
      const char *participant_id);

 private:
  enum class Mutation : uint8_t {
    ACTIVATE,
    SET_COMMUNITY_LEADER,
    QUARANTINE,
    REVOKE,
    FORGET,
  };

  bool expire_window_(uint32_t now_ms);
  void clear_candidate_();
  ParticipantEnrollmentResult persist_working_(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot);
  ParticipantEnrollmentResult mutate_(
      ParticipantRegistryStorageBackend &backend, ParticipantRegistryStore &store,
      ParticipantRegistrySnapshot *snapshot, const char *participant_id,
      Mutation mutation, bool value = false);
  static ParticipantEnrollmentResult registry_result_(ParticipantRegistryResult result);
  static bool valid_type_(ParticipantType type);

  ParticipantRegistry working_registry_{};
  ParticipantRecord candidate_{};
  uint32_t window_opened_at_ms_{0};
  uint32_t window_duration_ms_{0};
  bool window_open_{false};
};

static_assert(sizeof(ParticipantEnrollment) <= 1536,
              "participant enrollment workspace exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
