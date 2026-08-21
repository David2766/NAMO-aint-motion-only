#pragma once

#include "participant_pairing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#endif

namespace esphome {
namespace radar_api_server {

enum class ParticipantPairingOperation : uint8_t {
  NONE = 0,
  PREPARE,
  EXCHANGE,
  VERIFY,
  CONFIRM,
  CANCEL,
};

struct ParticipantPairingRuntimeSnapshot {
  uint32_t accepted_operation_id{0};
  uint32_t completed_operation_id{0};
  uint32_t prepare_rate_limited{0};
  ParticipantPairingOperation last_operation{ParticipantPairingOperation::NONE};
  ParticipantPairingResult operation_result{ParticipantPairingResult::OK};
  ParticipantPairingCommitResult commit_result{ParticipantPairingCommitResult::OK};
  ParticipantPairingState state{ParticipantPairingState::IDLE};
  ParticipantPairingRole role{ParticipantPairingRole::LEADER};
  uint32_t remaining_ms{0};
  uint32_t confirmation_code{0};
  bool busy{false};
  bool peer_verified{false};
  bool has_offer{false};
  bool has_proof{false};
  ParticipantPairingOffer offer{};
  std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> proof{};
  char peer_participant_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char leader_site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  bool has_invitation{false};
  bool invitation_rejected{false};
  uint32_t invitation_remaining_ms{0};
  char invitation_requester_address[64]{};
};

struct ParticipantPairingPendingCommit {
  ParticipantPairingCredential credential{};
  ParticipantPairingRole local_role{ParticipantPairingRole::LEADER};
  char leader_site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
};

class ParticipantPairingRuntime {
 public:
  bool queue_prepare(ParticipantPairingRole role, uint32_t duration_ms,
                     uint32_t now_ms,
                     uint32_t *operation_id);
  bool queue_exchange(const ParticipantPairingOffer &offer,
                      uint32_t *operation_id);
  bool queue_verify(
      const std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> &proof,
      uint32_t *operation_id);
  bool queue_confirm(uint32_t confirmation_code, uint32_t *operation_id);
  bool queue_cancel(uint32_t *operation_id);
  bool set_invitation(const char *requester_address, uint32_t now_ms,
                      uint32_t duration_ms);
  bool reject_invitation(uint32_t now_ms);
  void clear_invitation();

  bool process(ParticipantPairingCrypto &crypto,
               const PlatformIdentity &identity, uint32_t now_ms,
               ParticipantPairingPendingCommit *pending_commit);
  void complete_commit(ParticipantPairingCommitResult result, uint32_t now_ms);
  void expire(uint32_t now_ms);
  ParticipantPairingRuntimeSnapshot snapshot(uint32_t now_ms);

 private:
  struct Command {
    ParticipantPairingOperation operation{ParticipantPairingOperation::NONE};
    uint32_t operation_id{0};
    ParticipantPairingRole role{ParticipantPairingRole::LEADER};
    uint32_t value{0};
    ParticipantPairingOffer offer{};
    std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> proof{};
  };

  ParticipantPairingSession session_{};
  Command command_{};
  ParticipantPairingRuntimeSnapshot published_{};
  uint32_t next_operation_id_{1};
  uint32_t last_prepare_accepted_ms_{0};
  uint32_t last_publish_ms_{0};
  bool command_pending_{false};
  bool processing_{false};
  bool commit_pending_{false};
  bool invitation_active_{false};
  bool invitation_rejected_{false};
  bool prepare_seen_{false};
  uint32_t invitation_expires_at_ms_{0};
  char invitation_requester_address_[64]{};
#ifdef USE_ESP32
  portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif

  bool queue_(const Command &command, uint32_t *operation_id,
              uint32_t now_ms = 0);
  void publish_session_(uint32_t now_ms);
  void complete_operation_(const Command &command,
                           ParticipantPairingResult result,
                           ParticipantPairingCommitResult commit_result,
                           uint32_t now_ms);
  void lock_();
  void unlock_();
  void clear_invitation_locked_();
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
};

const char *participant_pairing_operation_to_string(
    ParticipantPairingOperation operation);

static_assert(std::is_trivially_copyable<ParticipantPairingRuntimeSnapshot>::value,
              "pairing runtime snapshots must remain fixed-size values");
static_assert(sizeof(ParticipantPairingRuntime) <= 2048,
              "pairing runtime exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
