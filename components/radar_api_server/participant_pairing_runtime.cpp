#include "participant_pairing_runtime.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

const char *participant_pairing_operation_to_string(
    ParticipantPairingOperation operation) {
  switch (operation) {
    case ParticipantPairingOperation::NONE: return "none";
    case ParticipantPairingOperation::PREPARE: return "prepare";
    case ParticipantPairingOperation::EXCHANGE: return "exchange";
    case ParticipantPairingOperation::VERIFY: return "verify";
    case ParticipantPairingOperation::CONFIRM: return "confirm";
    case ParticipantPairingOperation::CANCEL: return "cancel";
  }
  return "none";
}

bool ParticipantPairingRuntime::queue_prepare(ParticipantPairingRole role,
                                              uint32_t duration_ms,
                                              uint32_t now_ms,
                                              uint32_t *operation_id) {
  Command command{};
  command.operation = ParticipantPairingOperation::PREPARE;
  command.role = role;
  command.value = duration_ms;
  return this->queue_(command, operation_id, now_ms);
}

bool ParticipantPairingRuntime::queue_exchange(
    const ParticipantPairingOffer &offer, uint32_t *operation_id) {
  Command command{};
  command.operation = ParticipantPairingOperation::EXCHANGE;
  command.offer = offer;
  return this->queue_(command, operation_id);
}

bool ParticipantPairingRuntime::queue_verify(
    const std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> &proof,
    uint32_t *operation_id) {
  Command command{};
  command.operation = ParticipantPairingOperation::VERIFY;
  command.proof = proof;
  return this->queue_(command, operation_id);
}

bool ParticipantPairingRuntime::queue_confirm(uint32_t confirmation_code,
                                              uint32_t *operation_id) {
  Command command{};
  command.operation = ParticipantPairingOperation::CONFIRM;
  command.value = confirmation_code;
  return this->queue_(command, operation_id);
}

bool ParticipantPairingRuntime::queue_cancel(uint32_t *operation_id) {
  Command command{};
  command.operation = ParticipantPairingOperation::CANCEL;
  return this->queue_(command, operation_id);
}

bool ParticipantPairingRuntime::set_invitation(const char *requester_address,
                                               uint32_t now_ms,
                                               uint32_t duration_ms) {
  if (requester_address == nullptr || requester_address[0] == '\0' ||
      std::strlen(requester_address) >=
          sizeof(this->invitation_requester_address_) ||
      duration_ms == 0)
    return false;
  this->lock_();
  const bool pairing_available =
      !this->command_pending_ && !this->processing_ && !this->commit_pending_ &&
      (this->published_.state == ParticipantPairingState::IDLE ||
       this->published_.state == ParticipantPairingState::COMMITTED);
  if (!pairing_available ||
      (this->invitation_active_ &&
       !time_reached_(now_ms, this->invitation_expires_at_ms_) &&
       std::strcmp(this->invitation_requester_address_, requester_address) !=
           0)) {
    this->unlock_();
    return false;
  }
  std::strncpy(this->invitation_requester_address_, requester_address,
               sizeof(this->invitation_requester_address_) - 1);
  this->invitation_requester_address_[
      sizeof(this->invitation_requester_address_) - 1] = '\0';
  this->invitation_active_ = true;
  this->invitation_rejected_ = false;
  this->invitation_expires_at_ms_ = now_ms + duration_ms;
  this->unlock_();
  return true;
}

bool ParticipantPairingRuntime::reject_invitation(uint32_t now_ms) {
  this->lock_();
  if (!this->invitation_active_ ||
      time_reached_(now_ms, this->invitation_expires_at_ms_)) {
    this->clear_invitation_locked_();
    this->unlock_();
    return false;
  }
  this->invitation_rejected_ = true;
  this->invitation_expires_at_ms_ = now_ms + 15000U;
  this->unlock_();
  return true;
}

void ParticipantPairingRuntime::clear_invitation() {
  this->lock_();
  this->clear_invitation_locked_();
  this->unlock_();
}

bool ParticipantPairingRuntime::queue_(const Command &requested,
                                      uint32_t *operation_id,
                                      uint32_t now_ms) {
  if (operation_id == nullptr ||
      requested.operation == ParticipantPairingOperation::NONE)
    return false;
  this->lock_();
  if (this->command_pending_ || this->processing_ || this->commit_pending_) {
    this->unlock_();
    return false;
  }
  if (requested.operation == ParticipantPairingOperation::PREPARE) {
    const bool state_available =
        this->published_.state == ParticipantPairingState::IDLE ||
        this->published_.state == ParticipantPairingState::COMMITTED;
    const bool throttled =
        this->prepare_seen_ && now_ms - this->last_prepare_accepted_ms_ < 1000U;
    if (!state_available || throttled) {
      if (throttled)
        ++this->published_.prepare_rate_limited;
      this->unlock_();
      return false;
    }
    this->prepare_seen_ = true;
    this->last_prepare_accepted_ms_ = now_ms;
  }
  this->command_ = requested;
  this->command_.operation_id = this->next_operation_id_++;
  if (this->next_operation_id_ == 0)
    this->next_operation_id_ = 1;
  this->command_pending_ = true;
  this->published_.accepted_operation_id = this->command_.operation_id;
  this->published_.busy = true;
  *operation_id = this->command_.operation_id;
  this->unlock_();
  return true;
}

bool ParticipantPairingRuntime::process(
    ParticipantPairingCrypto &crypto, const PlatformIdentity &identity,
    uint32_t now_ms, ParticipantPairingPendingCommit *pending_commit) {
  if (pending_commit == nullptr)
    return false;
  Command command{};
  this->lock_();
  if (!this->command_pending_) {
    this->unlock_();
    return false;
  }
  command = this->command_;
  this->command_pending_ = false;
  this->processing_ = true;
  this->unlock_();

  ParticipantPairingResult result = ParticipantPairingResult::INVALID_STATE;
  switch (command.operation) {
    case ParticipantPairingOperation::PREPARE:
      result = this->session_.prepare(crypto, command.role, identity, now_ms,
                                      command.value);
      if (result == ParticipantPairingResult::OK)
        this->clear_invitation();
      break;
    case ParticipantPairingOperation::EXCHANGE:
      result = this->session_.exchange(crypto, command.offer, now_ms);
      break;
    case ParticipantPairingOperation::VERIFY:
      result = this->session_.verify_peer_proof(command.proof, now_ms);
      break;
    case ParticipantPairingOperation::CONFIRM: {
      ParticipantPairingCredential credential{};
      result = this->session_.credential(crypto, command.value, now_ms,
                                         &credential);
      if (result == ParticipantPairingResult::OK) {
        pending_commit->credential = credential;
        pending_commit->local_role = this->session_.role();
        std::strncpy(pending_commit->leader_site_id,
                     this->session_.leader_site_id(),
                     PlatformIdentity::MAX_ID_LENGTH);
        this->lock_();
        this->command_ = command;
        this->processing_ = false;
        this->commit_pending_ = true;
        this->unlock_();
        return true;
      }
      break;
    }
    case ParticipantPairingOperation::CANCEL:
      this->session_.cancel();
      this->clear_invitation();
      result = ParticipantPairingResult::OK;
      break;
    case ParticipantPairingOperation::NONE:
      break;
  }
  this->complete_operation_(command, result,
                            ParticipantPairingCommitResult::OK, now_ms);
  return false;
}

void ParticipantPairingRuntime::complete_commit(
    ParticipantPairingCommitResult result, uint32_t now_ms) {
  this->lock_();
  if (!this->commit_pending_) {
    this->unlock_();
    return;
  }
  const Command command = this->command_;
  this->commit_pending_ = false;
  this->unlock_();
  if (result == ParticipantPairingCommitResult::OK)
    this->session_.mark_committed();
  this->complete_operation_(command, ParticipantPairingResult::OK, result,
                            now_ms);
}

void ParticipantPairingRuntime::expire(uint32_t now_ms) {
  const ParticipantPairingState previous_state = this->session_.state();
  const bool active = this->session_.active(now_ms);
  const bool state_changed = previous_state != this->session_.state();
  if (!state_changed &&
      (!active || static_cast<uint32_t>(now_ms - this->last_publish_ms_) <
                      1000U))
    return;
  this->lock_();
  this->publish_session_(now_ms);
  this->last_publish_ms_ = now_ms;
  this->unlock_();
}

ParticipantPairingRuntimeSnapshot ParticipantPairingRuntime::snapshot(
    uint32_t now_ms) {
  this->lock_();
  if (this->invitation_active_ &&
      time_reached_(now_ms, this->invitation_expires_at_ms_))
    this->clear_invitation_locked_();
  auto snapshot = this->published_;
  snapshot.has_invitation = this->invitation_active_;
  snapshot.invitation_rejected = this->invitation_rejected_;
  snapshot.invitation_remaining_ms =
      this->invitation_active_
          ? this->invitation_expires_at_ms_ - now_ms
          : 0;
  std::strncpy(snapshot.invitation_requester_address,
               this->invitation_requester_address_,
               sizeof(snapshot.invitation_requester_address) - 1);
  this->unlock_();
  return snapshot;
}

void ParticipantPairingRuntime::complete_operation_(
    const Command &command, ParticipantPairingResult result,
    ParticipantPairingCommitResult commit_result, uint32_t now_ms) {
  this->lock_();
  this->published_.completed_operation_id = command.operation_id;
  this->published_.last_operation = command.operation;
  this->published_.operation_result = result;
  this->published_.commit_result = commit_result;
  this->published_.busy = false;
  this->processing_ = false;
  this->publish_session_(now_ms);
  this->last_publish_ms_ = now_ms;
  this->unlock_();
}

void ParticipantPairingRuntime::publish_session_(uint32_t now_ms) {
  this->published_.state = this->session_.state();
  this->published_.role = this->session_.role();
  this->published_.remaining_ms = this->session_.remaining_ms(now_ms);
  this->published_.confirmation_code = this->session_.confirmation_code();
  this->published_.peer_verified = this->session_.peer_verified();
  this->published_.has_offer =
      this->session_.state() == ParticipantPairingState::PREPARED ||
      this->session_.state() == ParticipantPairingState::EXCHANGED ||
      this->session_.state() == ParticipantPairingState::VERIFIED;
  this->published_.has_proof =
      this->session_.state() == ParticipantPairingState::EXCHANGED ||
      this->session_.state() == ParticipantPairingState::VERIFIED;
  this->published_.offer = this->published_.has_offer
                               ? this->session_.local_offer()
                               : ParticipantPairingOffer{};
  this->published_.proof = this->published_.has_proof
                               ? this->session_.local_proof()
                               : std::array<uint8_t,
                                            ParticipantPairingOffer::PROOF_SIZE>{};
  std::strncpy(this->published_.peer_participant_id,
               this->session_.peer_participant_id(),
               PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(this->published_.leader_site_id,
               this->session_.leader_site_id(),
               PlatformIdentity::MAX_ID_LENGTH);
}

void ParticipantPairingRuntime::lock_() {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->mutex_);
#endif
}

void ParticipantPairingRuntime::unlock_() {
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->mutex_);
#endif
}

void ParticipantPairingRuntime::clear_invitation_locked_() {
  this->invitation_active_ = false;
  this->invitation_rejected_ = false;
  this->invitation_expires_at_ms_ = 0;
  this->invitation_requester_address_[0] = '\0';
}

bool ParticipantPairingRuntime::time_reached_(uint32_t now_ms,
                                              uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

}  // namespace radar_api_server
}  // namespace esphome
