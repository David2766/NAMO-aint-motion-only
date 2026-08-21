#pragma once

#include "participant_credentials.h"
#include "participant_enrollment.h"
#include "participant_readiness.h"
#include "participant_registry_store.h"

#include <atomic>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

class RadarApiServer;
class PlatformParticipantOperation;

class PlatformParticipantOperationLease {
 public:
  PlatformParticipantOperationLease() = default;
  PlatformParticipantOperationLease(
      PlatformParticipantOperationLease &&other) noexcept;
  PlatformParticipantOperationLease &operator=(
      PlatformParticipantOperationLease &&other) noexcept;
  ~PlatformParticipantOperationLease();

  PlatformParticipantOperationLease(
      const PlatformParticipantOperationLease &) = delete;
  PlatformParticipantOperationLease &operator=(
      const PlatformParticipantOperationLease &) = delete;

  bool acquired() const { return this->owner_ != nullptr; }

 private:
  friend class PlatformParticipantOperation;
  explicit PlatformParticipantOperationLease(
      PlatformParticipantOperation *owner)
      : owner_(owner) {}
  void release_();

  PlatformParticipantOperation *owner_{nullptr};
};

enum class PlatformParticipantOperationAction : uint8_t {
  APPROVE = 0,
  CONFIGURE,
  RECONFIGURE,
  ACTIVATE,
  QUARANTINE,
  REVOKE,
  FORGET,
  SET_COMMUNITY_LEADER,
  INVALIDATE_READINESS,
};

enum class PlatformParticipantOperationResult : uint8_t {
  OK = 0,
  BUSY,
  UNAVAILABLE,
  CONTINUITY_UNAVAILABLE,
  ENROLLMENT_FAILED,
  ROSTER_PERSISTENCE_FAILED,
  ROLLBACK_FAILED,
};

struct PlatformParticipantOperationRequest {
  PlatformParticipantOperationAction action{
      PlatformParticipantOperationAction::CONFIGURE};
  const char *participant_id{nullptr};
  const ParticipantReadinessConfig *configuration{nullptr};
  ParticipantTrust approval_trust{ParticipantTrust::UNKNOWN};
  uint32_t now_ms{0};
  bool automatic_group{false};
  bool community_leader_eligible{false};
};

struct PlatformParticipantOperationOutcome {
  PlatformParticipantOperationResult result{
      PlatformParticipantOperationResult::UNAVAILABLE};
  ParticipantEnrollmentResult enrollment_result{
      ParticipantEnrollmentResult::PERSISTENCE_FAILED};
  bool rollback_attempted{false};
  bool rollback_succeeded{false};

  bool ok() const {
    return this->result == PlatformParticipantOperationResult::OK;
  }
};

struct PlatformParticipantOperationWorkspace {
  ParticipantRegistry participant_rollback{};
  ParticipantReadinessRegistry readiness_rollback{};
  ParticipantCredentialRegistry credential_rollback{};
};

static_assert(sizeof(PlatformParticipantOperationWorkspace) <= 10240,
              "platform participant operation workspace exceeded its budget");

class PlatformParticipantOperation {
 public:
  void setup(PlatformParticipantOperationWorkspace *workspace) {
    this->workspace_ = workspace;
    this->busy_.store(false, std::memory_order_release);
  }

  PlatformParticipantOperationLease try_acquire();
  bool busy() const { return this->busy_.load(std::memory_order_acquire); }
  ParticipantReadinessRegistry *readiness_scratch() {
    return this->workspace_ != nullptr
               ? &this->workspace_->readiness_rollback
               : nullptr;
  }

  PlatformParticipantOperationOutcome execute(
      RadarApiServer &server,
      const PlatformParticipantOperationRequest &request);

 private:
  friend class PlatformParticipantOperationLease;
  void release_();
  bool rollback_(RadarApiServer &server, bool registry_changed,
                 bool readiness_changed, bool credential_changed);

  PlatformParticipantOperationWorkspace *workspace_{nullptr};
  std::atomic<bool> busy_{false};
};

inline PlatformParticipantOperationLease::PlatformParticipantOperationLease(
    PlatformParticipantOperationLease &&other) noexcept
    : owner_(other.owner_) {
  other.owner_ = nullptr;
}

inline PlatformParticipantOperationLease &
PlatformParticipantOperationLease::operator=(
    PlatformParticipantOperationLease &&other) noexcept {
  if (this != &other) {
    this->release_();
    this->owner_ = other.owner_;
    other.owner_ = nullptr;
  }
  return *this;
}

inline PlatformParticipantOperationLease::~PlatformParticipantOperationLease() {
  this->release_();
}

inline void PlatformParticipantOperationLease::release_() {
  if (this->owner_ == nullptr)
    return;
  this->owner_->release_();
  this->owner_ = nullptr;
}

inline PlatformParticipantOperationLease
PlatformParticipantOperation::try_acquire() {
  bool expected = false;
  if (!this->busy_.compare_exchange_strong(
          expected, true, std::memory_order_acquire,
          std::memory_order_relaxed))
    return {};
  return PlatformParticipantOperationLease(this);
}

inline void PlatformParticipantOperation::release_() {
  this->busy_.store(false, std::memory_order_release);
}

const char *platform_participant_operation_action_to_string(
    PlatformParticipantOperationAction action);
const char *platform_participant_operation_result_to_string(
    PlatformParticipantOperationResult result);

}  // namespace radar_api_server
}  // namespace esphome
