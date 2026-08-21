#pragma once

#include "participant_credentials.h"
#include "platform_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class ParticipantPairingRole : uint8_t {
  LEADER = 0,
  JOINER = 1,
};

enum class ParticipantPairingState : uint8_t {
  IDLE = 0,
  PREPARED,
  EXCHANGED,
  VERIFIED,
  COMMITTED,
};

enum class ParticipantPairingResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_STATE,
  EXPIRED,
  ROLE_CONFLICT,
  IDENTITY_MISMATCH,
  CRYPTO_FAILED,
  PROOF_MISMATCH,
  CONFIRMATION_MISMATCH,
};

enum class ParticipantPairingCommitResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_STATE,
  REGISTRY_UNAVAILABLE,
  CREDENTIAL_STORAGE_UNAVAILABLE,
  SITE_CONFIGURATION_OWNER_REQUIRED,
  SITE_CONFLICT,
  SITE_STATE_INVALID,
  REGISTRY_FULL,
  PERSISTENCE_FAILED,
};

const char *participant_pairing_role_to_string(ParticipantPairingRole role);
const char *participant_pairing_state_to_string(ParticipantPairingState state);
const char *participant_pairing_result_to_string(ParticipantPairingResult result);
const char *participant_pairing_commit_result_to_string(
    ParticipantPairingCommitResult result);

struct ParticipantPairingOffer {
  static constexpr std::size_t SESSION_ID_SIZE = 16;
  static constexpr std::size_t NONCE_SIZE = 32;
  static constexpr std::size_t PRIVATE_KEY_SIZE = 32;
  static constexpr std::size_t PUBLIC_KEY_SIZE = 65;
  static constexpr std::size_t PROOF_SIZE = 32;

  ParticipantPairingRole role{ParticipantPairingRole::LEADER};
  char participant_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  std::array<uint8_t, SESSION_ID_SIZE> session_id{};
  std::array<uint8_t, NONCE_SIZE> nonce{};
  std::array<uint8_t, PUBLIC_KEY_SIZE> public_key{};
};

struct ParticipantPairingCredential {
  char participant_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint64_t participant_token{0};
  uint32_t key_version{0};
  std::array<uint8_t, 32> key{};
};

class ParticipantPairingCrypto {
 public:
  virtual ~ParticipantPairingCrypto() = default;
  virtual bool random_bytes(uint8_t *output, std::size_t size) = 0;
  virtual bool generate_key_pair(
      std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> *private_key,
      std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> *public_key) = 0;
  virtual bool derive_shared_secret(
      const std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> &private_key,
      const std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> &peer_public_key,
      std::array<uint8_t, 32> *shared_secret) = 0;
  virtual bool sha256(const uint8_t *data, std::size_t size,
                      std::array<uint8_t, 32> *digest) = 0;
  virtual bool hmac_sha256(const std::array<uint8_t, 32> &key,
                           const uint8_t *data, std::size_t size,
                           std::array<uint8_t, 32> *digest) = 0;
  virtual bool hkdf_sha256(const std::array<uint8_t, 32> &input_key,
                           const std::array<uint8_t, 32> &salt,
                           const uint8_t *info, std::size_t info_size,
                           std::array<uint8_t, 32> *output_key) = 0;
};

class ParticipantPairingSession {
 public:
  static constexpr uint32_t MAX_DURATION_MS = 5U * 60U * 1000U;

  ParticipantPairingResult prepare(ParticipantPairingCrypto &crypto,
                                   ParticipantPairingRole role,
                                   const PlatformIdentity &identity,
                                   uint32_t now_ms, uint32_t duration_ms);
  ParticipantPairingResult exchange(ParticipantPairingCrypto &crypto,
                                    const ParticipantPairingOffer &peer_offer,
                                    uint32_t now_ms);
  ParticipantPairingResult verify_peer_proof(
      const std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> &proof,
      uint32_t now_ms);
  ParticipantPairingResult credential(ParticipantPairingCrypto &crypto,
                                      uint32_t confirmation_code,
                                      uint32_t now_ms,
                                      ParticipantPairingCredential *credential) const;
  void mark_committed();
  void cancel();

  bool active(uint32_t now_ms);
  uint32_t remaining_ms(uint32_t now_ms) const;
  ParticipantPairingState state() const { return this->state_; }
  ParticipantPairingRole role() const { return this->local_offer_.role; }
  const ParticipantPairingOffer &local_offer() const { return this->local_offer_; }
  const char *peer_participant_id() const { return this->peer_participant_id_; }
  const char *leader_site_id() const { return this->leader_site_id_; }
  uint32_t confirmation_code() const { return this->confirmation_code_; }
  const std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> &local_proof() const {
    return this->local_proof_;
  }
  bool peer_verified() const { return this->peer_verified_; }

 private:
  ParticipantPairingState state_{ParticipantPairingState::IDLE};
  ParticipantPairingOffer local_offer_{};
  std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> private_key_{};
  std::array<uint8_t, 32> master_key_{};
  std::array<uint8_t, 32> transcript_hash_{};
  std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> local_proof_{};
  std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> expected_peer_proof_{};
  char peer_participant_id_[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char leader_site_id_[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t confirmation_code_{0};
  uint32_t opened_at_ms_{0};
  uint32_t duration_ms_{0};
  bool peer_verified_{false};

  bool expired_(uint32_t now_ms) const;
  void wipe_secrets_();
};

#ifdef USE_ESP32
class Esp32ParticipantPairingCrypto : public ParticipantPairingCrypto {
 public:
  bool random_bytes(uint8_t *output, std::size_t size) override;
  bool generate_key_pair(
      std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> *private_key,
      std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> *public_key) override;
  bool derive_shared_secret(
      const std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> &private_key,
      const std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> &peer_public_key,
      std::array<uint8_t, 32> *shared_secret) override;
  bool sha256(const uint8_t *data, std::size_t size,
              std::array<uint8_t, 32> *digest) override;
  bool hmac_sha256(const std::array<uint8_t, 32> &key,
                   const uint8_t *data, std::size_t size,
                   std::array<uint8_t, 32> *digest) override;
  bool hkdf_sha256(const std::array<uint8_t, 32> &input_key,
                   const std::array<uint8_t, 32> &salt,
                   const uint8_t *info, std::size_t info_size,
                   std::array<uint8_t, 32> *output_key) override;
};
#endif

static_assert(std::is_trivially_copyable<ParticipantPairingOffer>::value,
              "pairing offers must not own heap-backed state");
static_assert(sizeof(ParticipantPairingSession) <= 768,
              "pairing session exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
