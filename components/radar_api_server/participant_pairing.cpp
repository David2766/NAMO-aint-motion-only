#include "participant_pairing.h"

#include <algorithm>
#include <cstring>

#ifdef USE_ESP32
#include "esphome/core/helpers.h"

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t PAIRING_DOMAIN[] = {'N', 'A', 'M', 'O', '-', 'P', 'A', 'I',
                                      'R', '-', 'V', '1'};
constexpr uint8_t HKDF_INFO[] = {'N', 'A', 'M', 'O', ' ', 'm', 'e', 'm', 'b',
                                 'e', 'r', 's', 'h', 'i', 'p', ' ', 'v', '1'};

bool valid_identifier(const char *value) {
  if (value == nullptr || value[0] == '\0')
    return false;
  std::size_t length = 0;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (++length > PlatformIdentity::MAX_ID_LENGTH)
      return false;
    const char character = *cursor;
    const bool alphanumeric =
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9');
    if ((!alphanumeric && character != '-' && character != '_' &&
         character != '.' && character != ':') ||
        (cursor == value && !alphanumeric))
      return false;
  }
  return true;
}

bool all_zero(const uint8_t *value, std::size_t size) {
  uint8_t aggregate = 0;
  for (std::size_t index = 0; index < size; ++index)
    aggregate |= value[index];
  return aggregate == 0;
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                         std::size_t size) {
  uint8_t difference = 0;
  for (std::size_t index = 0; index < size; ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

bool append_bytes(uint8_t *buffer, std::size_t capacity, std::size_t *cursor,
                  const uint8_t *value, std::size_t size) {
  if (buffer == nullptr || cursor == nullptr || value == nullptr ||
      *cursor + size > capacity)
    return false;
  std::memcpy(buffer + *cursor, value, size);
  *cursor += size;
  return true;
}

bool append_identifier(uint8_t *buffer, std::size_t capacity,
                       std::size_t *cursor, const char *value) {
  if (!valid_identifier(value))
    return false;
  const std::size_t size = std::strlen(value);
  const uint8_t length = static_cast<uint8_t>(size);
  return append_bytes(buffer, capacity, cursor, &length, 1) &&
         append_bytes(buffer, capacity, cursor,
                      reinterpret_cast<const uint8_t *>(value), size);
}

bool append_labelled_hash_input(
    const std::array<uint8_t, 32> &transcript_hash, const char *label,
    std::array<uint8_t, 64> *input, std::size_t *input_size) {
  if (label == nullptr || input == nullptr || input_size == nullptr)
    return false;
  const std::size_t label_size = std::strlen(label);
  if (transcript_hash.size() + label_size > input->size())
    return false;
  std::memcpy(input->data(), transcript_hash.data(), transcript_hash.size());
  std::memcpy(input->data() + transcript_hash.size(), label, label_size);
  *input_size = transcript_hash.size() + label_size;
  return true;
}

uint64_t read_u64_be(const uint8_t *value) {
  uint64_t result = 0;
  for (uint8_t index = 0; index < 8; ++index)
    result = (result << 8U) | value[index];
  return result;
}

}  // namespace

const char *participant_pairing_role_to_string(ParticipantPairingRole role) {
  return role == ParticipantPairingRole::JOINER ? "joiner" : "leader";
}

const char *participant_pairing_state_to_string(ParticipantPairingState state) {
  switch (state) {
    case ParticipantPairingState::IDLE: return "idle";
    case ParticipantPairingState::PREPARED: return "prepared";
    case ParticipantPairingState::EXCHANGED: return "exchanged";
    case ParticipantPairingState::VERIFIED: return "verified";
    case ParticipantPairingState::COMMITTED: return "committed";
  }
  return "idle";
}

const char *participant_pairing_result_to_string(ParticipantPairingResult result) {
  switch (result) {
    case ParticipantPairingResult::OK: return "ok";
    case ParticipantPairingResult::INVALID_ARGUMENT: return "invalid_argument";
    case ParticipantPairingResult::INVALID_STATE: return "invalid_state";
    case ParticipantPairingResult::EXPIRED: return "expired";
    case ParticipantPairingResult::ROLE_CONFLICT: return "role_conflict";
    case ParticipantPairingResult::IDENTITY_MISMATCH: return "identity_mismatch";
    case ParticipantPairingResult::CRYPTO_FAILED: return "crypto_failed";
    case ParticipantPairingResult::PROOF_MISMATCH: return "proof_mismatch";
    case ParticipantPairingResult::CONFIRMATION_MISMATCH:
      return "confirmation_mismatch";
  }
  return "invalid_state";
}

const char *participant_pairing_commit_result_to_string(
    ParticipantPairingCommitResult result) {
  switch (result) {
    case ParticipantPairingCommitResult::OK: return "ok";
    case ParticipantPairingCommitResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case ParticipantPairingCommitResult::INVALID_STATE: return "invalid_state";
    case ParticipantPairingCommitResult::REGISTRY_UNAVAILABLE:
      return "registry_unavailable";
    case ParticipantPairingCommitResult::CREDENTIAL_STORAGE_UNAVAILABLE:
      return "credential_storage_unavailable";
    case ParticipantPairingCommitResult::SITE_CONFIGURATION_OWNER_REQUIRED:
      return "site_configuration_owner_required";
    case ParticipantPairingCommitResult::SITE_CONFLICT: return "site_conflict";
    case ParticipantPairingCommitResult::SITE_STATE_INVALID:
      return "site_state_invalid";
    case ParticipantPairingCommitResult::REGISTRY_FULL: return "registry_full";
    case ParticipantPairingCommitResult::PERSISTENCE_FAILED:
      return "persistence_failed";
  }
  return "invalid_state";
}

ParticipantPairingResult ParticipantPairingSession::prepare(
    ParticipantPairingCrypto &crypto, ParticipantPairingRole role,
    const PlatformIdentity &identity, uint32_t now_ms, uint32_t duration_ms) {
  if ((role != ParticipantPairingRole::LEADER &&
       role != ParticipantPairingRole::JOINER) ||
      !valid_identifier(identity.node_id) || !valid_identifier(identity.site_id) ||
      duration_ms == 0 || duration_ms > MAX_DURATION_MS)
    return ParticipantPairingResult::INVALID_ARGUMENT;

  this->cancel();
  this->local_offer_.role = role;
  std::strncpy(this->local_offer_.participant_id, identity.node_id,
               PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(this->local_offer_.site_id, identity.site_id,
               PlatformIdentity::MAX_ID_LENGTH);
  if (!crypto.random_bytes(this->local_offer_.session_id.data(),
                           this->local_offer_.session_id.size()) ||
      !crypto.random_bytes(this->local_offer_.nonce.data(),
                           this->local_offer_.nonce.size()) ||
      !crypto.generate_key_pair(&this->private_key_,
                                &this->local_offer_.public_key) ||
      all_zero(this->local_offer_.session_id.data(),
               this->local_offer_.session_id.size()) ||
      all_zero(this->local_offer_.nonce.data(),
               this->local_offer_.nonce.size())) {
    this->cancel();
    return ParticipantPairingResult::CRYPTO_FAILED;
  }
  this->opened_at_ms_ = now_ms;
  this->duration_ms_ = duration_ms;
  this->state_ = ParticipantPairingState::PREPARED;
  return ParticipantPairingResult::OK;
}

ParticipantPairingResult ParticipantPairingSession::exchange(
    ParticipantPairingCrypto &crypto,
    const ParticipantPairingOffer &peer_offer, uint32_t now_ms) {
  if (this->expired_(now_ms)) {
    this->cancel();
    return ParticipantPairingResult::EXPIRED;
  }
  if (this->state_ != ParticipantPairingState::PREPARED)
    return ParticipantPairingResult::INVALID_STATE;
  if (peer_offer.role == this->local_offer_.role)
    return ParticipantPairingResult::ROLE_CONFLICT;
  if (!valid_identifier(peer_offer.participant_id) ||
      !valid_identifier(peer_offer.site_id) ||
      std::strcmp(peer_offer.participant_id,
                  this->local_offer_.participant_id) == 0 ||
      constant_time_equal(peer_offer.session_id.data(),
                          this->local_offer_.session_id.data(),
                          peer_offer.session_id.size()) ||
      all_zero(peer_offer.session_id.data(), peer_offer.session_id.size()) ||
      all_zero(peer_offer.nonce.data(), peer_offer.nonce.size()) ||
      peer_offer.public_key[0] != 4 ||
      all_zero(peer_offer.public_key.data(), peer_offer.public_key.size()))
    return ParticipantPairingResult::IDENTITY_MISMATCH;

  const ParticipantPairingOffer &leader =
      this->local_offer_.role == ParticipantPairingRole::LEADER
          ? this->local_offer_
          : peer_offer;
  const ParticipantPairingOffer &joiner =
      this->local_offer_.role == ParticipantPairingRole::JOINER
          ? this->local_offer_
          : peer_offer;

  std::array<uint8_t, 512> transcript{};
  std::size_t cursor = 0;
  const uint8_t protocol_version = 1;
  if (!append_bytes(transcript.data(), transcript.size(), &cursor,
                    PAIRING_DOMAIN, sizeof(PAIRING_DOMAIN)) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    &protocol_version, 1) ||
      !append_identifier(transcript.data(), transcript.size(), &cursor,
                         leader.site_id) ||
      !append_identifier(transcript.data(), transcript.size(), &cursor,
                         leader.participant_id) ||
      !append_identifier(transcript.data(), transcript.size(), &cursor,
                         joiner.site_id) ||
      !append_identifier(transcript.data(), transcript.size(), &cursor,
                         joiner.participant_id) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    leader.session_id.data(), leader.session_id.size()) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    joiner.session_id.data(), joiner.session_id.size()) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    leader.nonce.data(), leader.nonce.size()) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    joiner.nonce.data(), joiner.nonce.size()) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    leader.public_key.data(), leader.public_key.size()) ||
      !append_bytes(transcript.data(), transcript.size(), &cursor,
                    joiner.public_key.data(), joiner.public_key.size()))
    return ParticipantPairingResult::INVALID_ARGUMENT;

  std::array<uint8_t, 32> shared_secret{};
  if (!crypto.derive_shared_secret(this->private_key_, peer_offer.public_key,
                                   &shared_secret) ||
      !crypto.sha256(transcript.data(), cursor, &this->transcript_hash_) ||
      !crypto.hkdf_sha256(shared_secret, this->transcript_hash_, HKDF_INFO,
                          sizeof(HKDF_INFO), &this->master_key_)) {
    shared_secret.fill(0);
    this->cancel();
    return ParticipantPairingResult::CRYPTO_FAILED;
  }
  shared_secret.fill(0);

  std::array<uint8_t, 64> proof_input{};
  std::size_t proof_size = 0;
  const char *local_label =
      this->local_offer_.role == ParticipantPairingRole::LEADER
          ? "leader-proof"
          : "joiner-proof";
  const char *peer_label =
      this->local_offer_.role == ParticipantPairingRole::LEADER
          ? "joiner-proof"
          : "leader-proof";
  if (!append_labelled_hash_input(this->transcript_hash_, local_label,
                                  &proof_input, &proof_size) ||
      !crypto.hmac_sha256(this->master_key_, proof_input.data(), proof_size,
                          &this->local_proof_) ||
      !append_labelled_hash_input(this->transcript_hash_, peer_label,
                                  &proof_input, &proof_size) ||
      !crypto.hmac_sha256(this->master_key_, proof_input.data(), proof_size,
                          &this->expected_peer_proof_) ||
      !append_labelled_hash_input(this->transcript_hash_, "confirmation",
                                  &proof_input, &proof_size))
  {
    this->cancel();
    return ParticipantPairingResult::CRYPTO_FAILED;
  }
  std::array<uint8_t, 32> confirmation{};
  if (!crypto.hmac_sha256(this->master_key_, proof_input.data(), proof_size,
                          &confirmation)) {
    this->cancel();
    return ParticipantPairingResult::CRYPTO_FAILED;
  }
  const uint32_t confirmation_value =
      (static_cast<uint32_t>(confirmation[0]) << 24U) |
      (static_cast<uint32_t>(confirmation[1]) << 16U) |
      (static_cast<uint32_t>(confirmation[2]) << 8U) |
      static_cast<uint32_t>(confirmation[3]);
  this->confirmation_code_ = confirmation_value % 1000000U;
  std::strncpy(this->peer_participant_id_, peer_offer.participant_id,
               PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(this->leader_site_id_, leader.site_id,
               PlatformIdentity::MAX_ID_LENGTH);
  this->peer_verified_ = false;
  this->state_ = ParticipantPairingState::EXCHANGED;
  return ParticipantPairingResult::OK;
}

ParticipantPairingResult ParticipantPairingSession::verify_peer_proof(
    const std::array<uint8_t, ParticipantPairingOffer::PROOF_SIZE> &proof,
    uint32_t now_ms) {
  if (this->expired_(now_ms)) {
    this->cancel();
    return ParticipantPairingResult::EXPIRED;
  }
  if (this->state_ != ParticipantPairingState::EXCHANGED)
    return ParticipantPairingResult::INVALID_STATE;
  if (!constant_time_equal(proof.data(), this->expected_peer_proof_.data(),
                           proof.size()))
    return ParticipantPairingResult::PROOF_MISMATCH;
  this->peer_verified_ = true;
  this->state_ = ParticipantPairingState::VERIFIED;
  return ParticipantPairingResult::OK;
}

ParticipantPairingResult ParticipantPairingSession::credential(
    ParticipantPairingCrypto &crypto, uint32_t confirmation_code,
    uint32_t now_ms,
    ParticipantPairingCredential *credential) const {
  if (credential == nullptr)
    return ParticipantPairingResult::INVALID_ARGUMENT;
  if (this->expired_(now_ms))
    return ParticipantPairingResult::EXPIRED;
  if (this->state_ != ParticipantPairingState::VERIFIED ||
      !this->peer_verified_)
    return ParticipantPairingResult::INVALID_STATE;
  if (confirmation_code != this->confirmation_code_)
    return ParticipantPairingResult::CONFIRMATION_MISMATCH;

  ParticipantPairingCredential generated{};
  std::strncpy(generated.participant_id, this->peer_participant_id_,
               PlatformIdentity::MAX_ID_LENGTH);
  generated.key_version = 1;

  std::array<uint8_t, 128> input{};
  const char key_label[] = "observation-key";
  const char token_label[] = "pair-token";
  std::memcpy(input.data(), key_label, sizeof(key_label) - 1);
  if (!crypto.hmac_sha256(this->master_key_, input.data(),
                          sizeof(key_label) - 1,
                          &generated.key))
    return ParticipantPairingResult::CRYPTO_FAILED;

  input.fill(0);
  std::memcpy(input.data(), token_label, sizeof(token_label) - 1);
  std::array<uint8_t, 32> token_material{};
  if (!crypto.hmac_sha256(generated.key, input.data(),
                          sizeof(token_label) - 1,
                          &token_material))
    return ParticipantPairingResult::CRYPTO_FAILED;
  generated.participant_token = read_u64_be(token_material.data());
  if (generated.participant_token == 0)
    generated.participant_token = 1;
  *credential = generated;
  return ParticipantPairingResult::OK;
}

void ParticipantPairingSession::mark_committed() {
  this->wipe_secrets_();
  this->state_ = ParticipantPairingState::COMMITTED;
  this->peer_verified_ = true;
}

void ParticipantPairingSession::cancel() {
  this->wipe_secrets_();
  this->state_ = ParticipantPairingState::IDLE;
  this->local_offer_ = ParticipantPairingOffer{};
  std::memset(this->peer_participant_id_, 0, sizeof(this->peer_participant_id_));
  std::memset(this->leader_site_id_, 0, sizeof(this->leader_site_id_));
  this->confirmation_code_ = 0;
  this->opened_at_ms_ = 0;
  this->duration_ms_ = 0;
  this->peer_verified_ = false;
}

bool ParticipantPairingSession::active(uint32_t now_ms) {
  if (this->state_ == ParticipantPairingState::IDLE ||
      this->state_ == ParticipantPairingState::COMMITTED)
    return false;
  if (this->expired_(now_ms)) {
    this->cancel();
    return false;
  }
  return true;
}

uint32_t ParticipantPairingSession::remaining_ms(uint32_t now_ms) const {
  if (this->state_ == ParticipantPairingState::IDLE ||
      this->state_ == ParticipantPairingState::COMMITTED ||
      this->expired_(now_ms))
    return 0;
  return this->duration_ms_ -
         static_cast<uint32_t>(now_ms - this->opened_at_ms_);
}

bool ParticipantPairingSession::expired_(uint32_t now_ms) const {
  return this->state_ != ParticipantPairingState::IDLE &&
         this->state_ != ParticipantPairingState::COMMITTED &&
         static_cast<uint32_t>(now_ms - this->opened_at_ms_) >=
             this->duration_ms_;
}

void ParticipantPairingSession::wipe_secrets_() {
  volatile uint8_t *private_bytes = this->private_key_.data();
  volatile uint8_t *master_bytes = this->master_key_.data();
  for (std::size_t index = 0; index < this->private_key_.size(); ++index)
    private_bytes[index] = 0;
  for (std::size_t index = 0; index < this->master_key_.size(); ++index)
    master_bytes[index] = 0;
  this->transcript_hash_.fill(0);
  this->local_proof_.fill(0);
  this->expected_peer_proof_.fill(0);
}

#ifdef USE_ESP32

namespace {

int pairing_random(void *, unsigned char *output, std::size_t size) {
  return esphome::random_bytes(output, size) ? 0 : -1;
}

}  // namespace

bool Esp32ParticipantPairingCrypto::random_bytes(uint8_t *output,
                                                 std::size_t size) {
  return output != nullptr && size != 0 && esphome::random_bytes(output, size);
}

bool Esp32ParticipantPairingCrypto::generate_key_pair(
    std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> *private_key,
    std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> *public_key) {
  if (private_key == nullptr || public_key == nullptr)
    return false;
  mbedtls_ecp_group group;
  mbedtls_mpi private_value;
  mbedtls_ecp_point public_value;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&private_value);
  mbedtls_ecp_point_init(&public_value);
  std::size_t public_size = 0;
  const bool ok =
      mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecdh_gen_public(&group, &private_value, &public_value,
                              pairing_random, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&private_value, private_key->data(),
                               private_key->size()) == 0 &&
      mbedtls_ecp_point_write_binary(
          &group, &public_value, MBEDTLS_ECP_PF_UNCOMPRESSED, &public_size,
          public_key->data(), public_key->size()) == 0 &&
      public_size == public_key->size();
  mbedtls_ecp_point_free(&public_value);
  mbedtls_mpi_free(&private_value);
  mbedtls_ecp_group_free(&group);
  if (!ok) {
    private_key->fill(0);
    public_key->fill(0);
  }
  return ok;
}

bool Esp32ParticipantPairingCrypto::derive_shared_secret(
    const std::array<uint8_t, ParticipantPairingOffer::PRIVATE_KEY_SIZE> &private_key,
    const std::array<uint8_t, ParticipantPairingOffer::PUBLIC_KEY_SIZE> &peer_public_key,
    std::array<uint8_t, 32> *shared_secret) {
  if (shared_secret == nullptr)
    return false;
  mbedtls_ecp_group group;
  mbedtls_mpi private_value;
  mbedtls_mpi shared_value;
  mbedtls_ecp_point peer_value;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&private_value);
  mbedtls_mpi_init(&shared_value);
  mbedtls_ecp_point_init(&peer_value);
  const bool ok =
      mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_mpi_read_binary(&private_value, private_key.data(),
                              private_key.size()) == 0 &&
      mbedtls_ecp_point_read_binary(&group, &peer_value,
                                    peer_public_key.data(),
                                    peer_public_key.size()) == 0 &&
      mbedtls_ecp_check_pubkey(&group, &peer_value) == 0 &&
      mbedtls_ecdh_compute_shared(&group, &shared_value, &peer_value,
                                  &private_value, pairing_random, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&shared_value, shared_secret->data(),
                               shared_secret->size()) == 0;
  mbedtls_ecp_point_free(&peer_value);
  mbedtls_mpi_free(&shared_value);
  mbedtls_mpi_free(&private_value);
  mbedtls_ecp_group_free(&group);
  if (!ok)
    shared_secret->fill(0);
  return ok;
}

bool Esp32ParticipantPairingCrypto::sha256(
    const uint8_t *data, std::size_t size,
    std::array<uint8_t, 32> *digest) {
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return data != nullptr && digest != nullptr && info != nullptr &&
         mbedtls_md(info, data, size, digest->data()) == 0;
}

bool Esp32ParticipantPairingCrypto::hmac_sha256(
    const std::array<uint8_t, 32> &key, const uint8_t *data,
    std::size_t size, std::array<uint8_t, 32> *digest) {
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return data != nullptr && digest != nullptr && info != nullptr &&
         mbedtls_md_hmac(info, key.data(), key.size(), data, size,
                         digest->data()) == 0;
}

bool Esp32ParticipantPairingCrypto::hkdf_sha256(
    const std::array<uint8_t, 32> &input_key,
    const std::array<uint8_t, 32> &salt, const uint8_t *info,
    std::size_t info_size, std::array<uint8_t, 32> *output_key) {
  const mbedtls_md_info_t *md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr && output_key != nullptr && md != nullptr &&
         mbedtls_hkdf(md, salt.data(), salt.size(), input_key.data(),
                      input_key.size(), info, info_size, output_key->data(),
                      output_key->size()) == 0;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
