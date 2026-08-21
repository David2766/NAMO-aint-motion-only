#include "site_replication.h"

#include "role_quorum.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef USE_ESP32
#include "esphome/components/network/util.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_PSRAM
#include <esp_heap_caps.h>
#endif
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

#if defined(USE_ESP32_FRAMEWORK_ESP_IDF) && !defined(MBEDTLS_AES_ALT)
#error "ESP-IDF mbedTLS AES configuration is required"
#endif

#include <cerrno>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t REPLICATION_VERSION = 1;

bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                         std::size_t size) {
  volatile uint8_t difference = 0;
  for (std::size_t index = 0; index < size; ++index)
    difference |= static_cast<uint8_t>(left[index] ^ right[index]);
  return difference == 0;
}

void write_u32_be(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value >> 24U);
  target[1] = static_cast<uint8_t>(value >> 16U);
  target[2] = static_cast<uint8_t>(value >> 8U);
  target[3] = static_cast<uint8_t>(value);
}

void write_u64_be(uint8_t *target, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index)
    target[index] =
        static_cast<uint8_t>(value >> ((7U - index) * 8U));
}

uint32_t read_u32_be(const uint8_t *source) {
  return (static_cast<uint32_t>(source[0]) << 24U) |
         (static_cast<uint32_t>(source[1]) << 16U) |
         (static_cast<uint32_t>(source[2]) << 8U) |
         static_cast<uint32_t>(source[3]);
}

uint64_t read_u64_be(const uint8_t *source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index)
    value = (value << 8U) | source[index];
  return value;
}

bool valid_kind(SiteReplicationKind kind) {
  return kind == SiteReplicationKind::REQUEST ||
         kind == SiteReplicationKind::RESPONSE;
}

bool valid_target(SiteReplicationTarget target) {
  return target == SiteReplicationTarget::SITE_CONFIGURATION ||
         target == SiteReplicationTarget::FLOORPLAN_CONFIG ||
         target == SiteReplicationTarget::FLOORPLAN_IMAGE ||
         target == SiteReplicationTarget::SITE_DIRECTORY ||
         target == SiteReplicationTarget::CREDENTIAL_BUNDLE;
}

bool valid_status(SiteReplicationWireStatus status) {
  return status >= SiteReplicationWireStatus::OK &&
         status <= SiteReplicationWireStatus::PARTICIPANT_REJECTED;
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  target[0] = '\0';
  if (source == nullptr)
    return;
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t value = 2166136261UL;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= data[index];
    value *= 16777619UL;
  }
  return value;
}

#ifdef USE_ESP32

const char *const TAG = "site_replication";

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool hmac_sha256(const std::array<uint8_t, 32> &key, const uint8_t *data,
                 std::size_t size, uint8_t *output) {
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr &&
         mbedtls_md_hmac(info, key.data(), key.size(), data, size, output) == 0;
}

bool credential_request_claim(const ParticipantCredentialRecord &credential,
                              const char *participant_id, uint32_t *high,
                              uint32_t *low) {
  if (participant_id == nullptr || participant_id[0] == '\0' ||
      high == nullptr || low == nullptr)
    return false;
  std::array<uint8_t, 32> digest{};
  if (!hmac_sha256(
          credential.key,
          reinterpret_cast<const uint8_t *>(participant_id),
          std::strlen(participant_id), digest.data()))
    return false;
  *high = read_u32_be(digest.data());
  *low = read_u32_be(digest.data() + sizeof(uint32_t));
  return true;
}

bool crypt_credential_chunk(const std::array<uint8_t, 32> &key,
                            uint64_t participant_token, uint32_t request_id,
                            uint32_t offset, uint8_t *data,
                            std::size_t size) {
  if (data == nullptr || size == 0 || request_id == 0)
    return false;
  std::array<uint8_t, 24> nonce_input{};
  std::memcpy(nonce_input.data(), "NAMO-CRED-V1", 12);
  write_u64_be(nonce_input.data() + 12, participant_token);
  write_u32_be(nonce_input.data() + 20, request_id ^ offset);
  std::array<uint8_t, 32> nonce_hash{};
  if (!hmac_sha256(key, nonce_input.data(), nonce_input.size(),
                   nonce_hash.data()))
    return false;
  std::array<uint8_t, 16> nonce_counter{};
  std::copy_n(nonce_hash.begin(), nonce_counter.size(),
              nonce_counter.begin());
  std::array<uint8_t, 16> stream_block{};
  std::size_t nonce_offset = 0;
  mbedtls_aes_context aes{};
  mbedtls_aes_init(&aes);
  const bool ready =
      mbedtls_aes_setkey_enc(&aes, key.data(), key.size() * 8U) == 0;
  const int result = ready
                         ? mbedtls_aes_crypt_ctr(
                               &aes, size, &nonce_offset,
                               nonce_counter.data(), stream_block.data(), data,
                               data)
                         : -1;
  mbedtls_aes_free(&aes);
  return result == 0;
}

RadarPayloadTarget storage_target(SiteReplicationTarget target) {
  return target == SiteReplicationTarget::FLOORPLAN_CONFIG
             ? RadarPayloadTarget::CONFIG
             : RadarPayloadTarget::IMAGE;
}

#endif

}  // namespace

SiteReplicationSourceAdmission SiteReplicationSourceLease::admit(
    const char *participant_id, uint32_t source_ipv4, uint32_t now_ms) {
  if (participant_id == nullptr || participant_id[0] == '\0' ||
      source_ipv4 == 0)
    return SiteReplicationSourceAdmission::INVALID_ARGUMENT;

  this->expire(now_ms);
  if (this->diagnostics_.active) {
    if (this->source_ipv4_ == source_ipv4 &&
        std::strcmp(this->diagnostics_.participant_id, participant_id) == 0) {
      this->expires_ms_ = now_ms + LEASE_MS;
      return SiteReplicationSourceAdmission::CONTINUED;
    }
    ++this->diagnostics_.busy;
    return SiteReplicationSourceAdmission::BUSY;
  }

  this->diagnostics_.active = true;
  copy_text(this->diagnostics_.participant_id,
            sizeof(this->diagnostics_.participant_id), participant_id);
  this->source_ipv4_ = source_ipv4;
  this->expires_ms_ = now_ms + LEASE_MS;
  ++this->diagnostics_.accepted;
  return SiteReplicationSourceAdmission::ACQUIRED;
}

bool SiteReplicationSourceLease::release(const char *participant_id,
                                         uint32_t source_ipv4,
                                         bool completed) {
  if (!this->diagnostics_.active || participant_id == nullptr ||
      this->source_ipv4_ != source_ipv4 ||
      std::strcmp(this->diagnostics_.participant_id, participant_id) != 0)
    return false;
  if (completed)
    ++this->diagnostics_.completed;
  this->clear_active_();
  return true;
}

bool SiteReplicationSourceLease::expire(uint32_t now_ms) {
  if (!this->diagnostics_.active ||
      static_cast<int32_t>(now_ms - this->expires_ms_) < 0)
    return false;
  ++this->diagnostics_.expired;
  this->clear_active_();
  return true;
}

uint32_t SiteReplicationSourceLease::remaining_ms(uint32_t now_ms) const {
  if (!this->diagnostics_.active ||
      static_cast<int32_t>(now_ms - this->expires_ms_) >= 0)
    return 0;
  return this->expires_ms_ - now_ms;
}

void SiteReplicationSourceLease::clear_active_() {
  this->diagnostics_.active = false;
  this->diagnostics_.participant_id[0] = '\0';
  this->source_ipv4_ = 0;
  this->expires_ms_ = 0;
}

SiteReplicationReplayAdmission SiteReplicationReplayCache::classify(
    uint64_t participant_token, uint32_t source_ipv4,
    const SiteReplicationFrame &request, uint32_t now_ms) {
  if (this->valid_ &&
      static_cast<int32_t>(now_ms - this->expires_at_ms_) >= 0)
    this->clear_();
  if (!this->valid_ || this->participant_token_ != participant_token ||
      this->source_ipv4_ != source_ipv4)
    return SiteReplicationReplayAdmission::NEW_REQUEST;

  if (request.request_id == this->request_id_ &&
      request.target == this->target_ && request.offset == this->offset_ &&
      request.length == this->length_)
    return SiteReplicationReplayAdmission::DUPLICATE;
  if (static_cast<int32_t>(request.request_id - this->request_id_) <= 0)
    return SiteReplicationReplayAdmission::STALE;
  return SiteReplicationReplayAdmission::NEW_REQUEST;
}

void SiteReplicationReplayCache::remember(
    uint64_t participant_token, uint32_t source_ipv4,
    const SiteReplicationFrame &request, const uint8_t *packet,
    std::size_t packet_size, uint32_t now_ms) {
  if (participant_token == 0 || source_ipv4 == 0 || packet == nullptr ||
      packet_size == 0 || packet_size > this->packet_.size()) {
    this->clear_();
    return;
  }
  this->valid_ = true;
  this->participant_token_ = participant_token;
  this->source_ipv4_ = source_ipv4;
  this->request_id_ = request.request_id;
  this->offset_ = request.offset;
  this->length_ = request.length;
  this->expires_at_ms_ = now_ms + SiteReplicationSourceLease::LEASE_MS;
  this->target_ = request.target;
  this->packet_size_ = static_cast<uint16_t>(packet_size);
  std::memcpy(this->packet_.data(), packet, packet_size);
}

void SiteReplicationReplayCache::clear_() {
  this->valid_ = false;
  this->participant_token_ = 0;
  this->source_ipv4_ = 0;
  this->request_id_ = 0;
  this->offset_ = 0;
  this->length_ = 0;
  this->expires_at_ms_ = 0;
  this->target_ = SiteReplicationTarget::SITE_CONFIGURATION;
  this->packet_size_ = 0;
}

SiteReplicationCodecResult encode_site_replication_frame(
    const SiteReplicationFrame &frame, uint8_t *packet,
    std::size_t packet_capacity, std::size_t *signed_size,
    std::size_t *packet_size) {
  if (packet == nullptr || signed_size == nullptr || packet_size == nullptr ||
      !valid_kind(frame.kind) || !valid_target(frame.target) ||
      !valid_status(frame.status) || frame.key_version == 0 ||
      frame.participant_token == 0 || frame.request_id == 0 ||
      frame.length > SiteReplicationEnvelope::MAX_CHUNK_SIZE ||
      (frame.kind == SiteReplicationKind::REQUEST &&
       (frame.body != nullptr || frame.status != SiteReplicationWireStatus::OK)) ||
      (frame.kind == SiteReplicationKind::RESPONSE && frame.length > 0 &&
       frame.body == nullptr))
    return SiteReplicationCodecResult::INVALID_ARGUMENT;

  const std::size_t body_size =
      frame.kind == SiteReplicationKind::RESPONSE ? frame.length : 0;
  const std::size_t signed_length =
      SiteReplicationEnvelope::PREFIX_SIZE + body_size;
  const std::size_t total_size =
      signed_length + SiteReplicationEnvelope::TAG_SIZE;
  if (total_size > packet_capacity ||
      total_size > SiteReplicationEnvelope::MAX_PACKET_SIZE)
    return SiteReplicationCodecResult::BUFFER_TOO_SMALL;

  std::memcpy(packet, "NMSR", 4);
  packet[4] = REPLICATION_VERSION;
  packet[5] = static_cast<uint8_t>(frame.kind);
  packet[6] = static_cast<uint8_t>(SiteReplicationEnvelope::PREFIX_SIZE);
  packet[7] = static_cast<uint8_t>(frame.target);
  write_u32_be(packet + 8, frame.key_version);
  write_u64_be(packet + 12, frame.participant_token);
  write_u32_be(packet + 20, frame.request_id);
  write_u32_be(packet + 24, frame.offset);
  write_u32_be(packet + 28, frame.length);
  write_u32_be(packet + 32, frame.total_size);
  write_u32_be(packet + 36, frame.payload_hash);
  write_u32_be(packet + 40, frame.site_generation);
  packet[44] = static_cast<uint8_t>(frame.status);
  packet[45] = 0;
  packet[46] = 0;
  packet[47] = 0;
  if (body_size > 0 && frame.body != packet + SiteReplicationEnvelope::PREFIX_SIZE)
    std::memmove(packet + SiteReplicationEnvelope::PREFIX_SIZE, frame.body,
                 body_size);
  *signed_size = signed_length;
  *packet_size = total_size;
  return SiteReplicationCodecResult::OK;
}

SiteReplicationCodecResult parse_site_replication_frame(
    const uint8_t *packet, std::size_t packet_size,
    SiteReplicationEnvelope *envelope) {
  if (packet == nullptr || envelope == nullptr ||
      packet_size < SiteReplicationEnvelope::PREFIX_SIZE +
                        SiteReplicationEnvelope::TAG_SIZE ||
      packet_size > SiteReplicationEnvelope::MAX_PACKET_SIZE ||
      std::memcmp(packet, "NMSR", 4) != 0 ||
      packet[4] != REPLICATION_VERSION ||
      packet[6] != SiteReplicationEnvelope::PREFIX_SIZE || packet[45] != 0 ||
      packet[46] != 0 || packet[47] != 0)
    return SiteReplicationCodecResult::INVALID_PACKET;

  SiteReplicationEnvelope parsed{};
  parsed.frame.kind = static_cast<SiteReplicationKind>(packet[5]);
  parsed.frame.target = static_cast<SiteReplicationTarget>(packet[7]);
  parsed.frame.key_version = read_u32_be(packet + 8);
  parsed.frame.participant_token = read_u64_be(packet + 12);
  parsed.frame.request_id = read_u32_be(packet + 20);
  parsed.frame.offset = read_u32_be(packet + 24);
  parsed.frame.length = read_u32_be(packet + 28);
  parsed.frame.total_size = read_u32_be(packet + 32);
  parsed.frame.payload_hash = read_u32_be(packet + 36);
  parsed.frame.site_generation = read_u32_be(packet + 40);
  parsed.frame.status = static_cast<SiteReplicationWireStatus>(packet[44]);
  if (!valid_kind(parsed.frame.kind) || !valid_target(parsed.frame.target) ||
      !valid_status(parsed.frame.status) || parsed.frame.key_version == 0 ||
      parsed.frame.participant_token == 0 || parsed.frame.request_id == 0 ||
      parsed.frame.length > SiteReplicationEnvelope::MAX_CHUNK_SIZE)
    return SiteReplicationCodecResult::INVALID_PACKET;
  const std::size_t body_size =
      parsed.frame.kind == SiteReplicationKind::RESPONSE
          ? parsed.frame.length
          : 0;
  const std::size_t signed_size =
      SiteReplicationEnvelope::PREFIX_SIZE + body_size;
  if (packet_size != signed_size + SiteReplicationEnvelope::TAG_SIZE)
    return SiteReplicationCodecResult::INVALID_PACKET;
  parsed.frame.body = body_size > 0
                          ? packet + SiteReplicationEnvelope::PREFIX_SIZE
                          : nullptr;
  parsed.signed_data = packet;
  parsed.signed_size = signed_size;
  parsed.tag = packet + signed_size;
  *envelope = parsed;
  return SiteReplicationCodecResult::OK;
}

const char *site_replication_state_to_string(SiteReplicationState state) {
  switch (state) {
    case SiteReplicationState::IDLE: return "idle";
    case SiteReplicationState::WAITING_FOR_OWNER: return "waiting_for_owner";
    case SiteReplicationState::SITE_CONFIGURATION: return "site_configuration";
    case SiteReplicationState::DIRECTORY_METADATA:
      return "directory_metadata";
    case SiteReplicationState::DIRECTORY_PAYLOAD:
      return "directory_payload";
    case SiteReplicationState::DIRECTORY_COMMITTING:
      return "directory_committing";
    case SiteReplicationState::CREDENTIAL_METADATA:
      return "credential_metadata";
    case SiteReplicationState::CREDENTIAL_PAYLOAD:
      return "credential_payload";
    case SiteReplicationState::CREDENTIAL_COMMITTING:
      return "credential_committing";
    case SiteReplicationState::CONFIG_METADATA: return "config_metadata";
    case SiteReplicationState::IMAGE_METADATA: return "image_metadata";
    case SiteReplicationState::CONFIG_PAYLOAD: return "config_payload";
    case SiteReplicationState::IMAGE_PAYLOAD: return "image_payload";
    case SiteReplicationState::COMMITTING: return "committing";
    case SiteReplicationState::COMPLETE: return "complete";
    case SiteReplicationState::CONFLICT: return "conflict";
    case SiteReplicationState::FAILED: return "failed";
  }
  return "failed";
}

const char *site_replication_result_to_string(SiteReplicationResult result) {
  switch (result) {
    case SiteReplicationResult::IDLE: return "idle";
    case SiteReplicationResult::OK: return "ok";
    case SiteReplicationResult::BUSY: return "busy";
    case SiteReplicationResult::INVALID_OWNER: return "invalid_owner";
    case SiteReplicationResult::OWNER_OFFLINE: return "owner_offline";
    case SiteReplicationResult::CREDENTIAL_MISSING: return "credential_missing";
    case SiteReplicationResult::SOCKET_ERROR: return "socket_error";
    case SiteReplicationResult::TIMEOUT: return "timeout";
    case SiteReplicationResult::INVALID_PACKET: return "invalid_packet";
    case SiteReplicationResult::AUTHENTICATION_FAILED: return "authentication_failed";
    case SiteReplicationResult::REMOTE_REJECTED: return "remote_rejected";
    case SiteReplicationResult::INVALID_CONFIGURATION: return "invalid_configuration";
    case SiteReplicationResult::REVISION_CONFLICT: return "revision_conflict";
    case SiteReplicationResult::PAYLOAD_MISMATCH: return "payload_mismatch";
    case SiteReplicationResult::STORAGE_FAILED: return "storage_failed";
  }
  return "failed";
}

const char *site_replication_failure_detail_to_string(
    SiteReplicationFailureDetail detail) {
  switch (detail) {
    case SiteReplicationFailureDetail::NONE: return "none";
    case SiteReplicationFailureDetail::CREDENTIAL_PROBE_REJECTED:
      return "credential_probe_rejected";
    case SiteReplicationFailureDetail::CREDENTIAL_PROBE_TIMEOUT:
      return "credential_probe_timeout";
    case SiteReplicationFailureDetail::CREDENTIAL_PAYLOAD_CRYPTO_FAILED:
      return "credential_payload_crypto_failed";
    case SiteReplicationFailureDetail::CREDENTIAL_BUNDLE_BINDING_MISMATCH:
      return "credential_bundle_binding_mismatch";
    case SiteReplicationFailureDetail::CREDENTIAL_REQUEST_CLAIM_FAILED:
      return "credential_request_claim_failed";
    case SiteReplicationFailureDetail::CREDENTIAL_REQUEST_ENCODING_FAILED:
      return "credential_request_encoding_failed";
    case SiteReplicationFailureDetail::CREDENTIAL_REQUEST_HMAC_FAILED:
      return "credential_request_hmac_failed";
  }
  return "none";
}

const char *site_replication_target_to_string(SiteReplicationTarget target) {
  switch (target) {
    case SiteReplicationTarget::SITE_CONFIGURATION:
      return "floorplan";
    case SiteReplicationTarget::FLOORPLAN_CONFIG:
      return "floorplan_config";
    case SiteReplicationTarget::FLOORPLAN_IMAGE:
      return "floorplan_image";
    case SiteReplicationTarget::SITE_DIRECTORY:
      return "site_directory";
    case SiteReplicationTarget::CREDENTIAL_BUNDLE:
      return "credential_bundle";
  }
  return "unknown";
}

bool site_replication_session_matches(
    const SiteReplicationDiagnostics &diagnostics, const char *owner_node_id,
    SiteConfigurationReplicationMode mode) {
  const bool active =
      diagnostics.state != SiteReplicationState::IDLE &&
      diagnostics.state != SiteReplicationState::COMPLETE &&
      diagnostics.state != SiteReplicationState::FAILED &&
      diagnostics.state != SiteReplicationState::CONFLICT;
  return active && owner_node_id != nullptr && owner_node_id[0] != '\0' &&
         diagnostics.target == SiteReplicationTarget::SITE_CONFIGURATION &&
         diagnostics.mode == mode &&
         std::strcmp(diagnostics.owner_node_id, owner_node_id) == 0;
}

SiteReplicationResult site_replication_response_result(
    SiteReplicationWireStatus status, uint32_t payload_hash) {
  if (status == SiteReplicationWireStatus::OK)
    return SiteReplicationResult::OK;
  if (status == SiteReplicationWireStatus::STORAGE_FAILED &&
      payload_hash == SITE_REPLICATION_BUSY_PAYLOAD_HASH)
    return SiteReplicationResult::BUSY;
  return SiteReplicationResult::REMOTE_REJECTED;
}

bool site_replication_response_matches_request(
    const SiteReplicationFrame &frame, uint32_t expected_request_id,
    SiteReplicationTarget expected_target) {
  return frame.kind == SiteReplicationKind::RESPONSE &&
         frame.request_id == expected_request_id &&
         frame.target == expected_target;
}

uint32_t site_replication_slot_delay_ms(
    const SiteConfigurationSnapshot &configuration,
    const char *local_node_id) {
  return site_replication_source_slot_delay_ms(
      configuration, configuration.configuration_owner_node_id,
      local_node_id);
}

uint32_t site_replication_source_slot_delay_ms(
    const SiteConfigurationSnapshot &configuration,
    const char *source_node_id, const char *local_node_id) {
  if (source_node_id == nullptr || source_node_id[0] == '\0' ||
      local_node_id == nullptr || local_node_id[0] == '\0' ||
      std::strcmp(local_node_id, source_node_id) == 0)
    return std::numeric_limits<uint32_t>::max();

  uint32_t follower_slot = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, source_node_id) == 0)
      continue;
    if (std::strcmp(member.participant_id, local_node_id) == 0)
      return (follower_slot + 1U) * SITE_REPLICATION_SLOT_MS;
    ++follower_slot;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t site_replication_retry_delay_ms(uint8_t retry_count) {
  if (retry_count == 0)
    return SITE_REPLICATION_SLOT_MS;
  uint32_t delay_ms = SITE_REPLICATION_SLOT_MS;
  for (uint8_t attempt = 1; attempt < retry_count && delay_ms < 30000U;
       ++attempt)
    delay_ms = delay_ms >= 15000U ? 30000U : delay_ms * 2U;
  return delay_ms > 30000U ? 30000U : delay_ms;
}

PairingSiteSyncDecision pairing_site_sync_decision(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRecord *participant,
    const ParticipantCredentialRecord *credential,
    const ParticipantEndpoint *endpoint, const char *local_node_id,
    const char *requested_owner_node_id, bool local_floorplan_replica_ready,
    uint32_t now_ms) {
  const auto *local_member =
      local_node_id != nullptr ? configuration.find(local_node_id) : nullptr;
  const bool pairing_relationship =
      participant != nullptr &&
      participant->membership == ParticipantMembership::QUARANTINED;
  const bool activation_continuity =
      participant != nullptr &&
      participant->membership == ParticipantMembership::ACTIVE &&
      local_member != nullptr &&
      local_member->type == ParticipantType::NAMO_NODE &&
      local_member->membership == ParticipantMembership::QUARANTINED;
  if (local_node_id == nullptr || local_node_id[0] == '\0' ||
      requested_owner_node_id == nullptr ||
      requested_owner_node_id[0] == '\0' ||
      std::strcmp(configuration.configuration_owner_node_id,
                  requested_owner_node_id) != 0 ||
      participant == nullptr || credential == nullptr ||
      participant->type != ParticipantType::NAMO_NODE ||
      participant->trust != ParticipantTrust::COMMUNITY_UNVERIFIED ||
      (!pairing_relationship && !activation_continuity) ||
      std::strcmp(participant->participant_id, requested_owner_node_id) != 0 ||
      !credential->enabled ||
      std::strcmp(credential->participant_id, requested_owner_node_id) != 0)
    return PairingSiteSyncDecision::NOT_APPLICABLE;

  if (endpoint == nullptr || !endpoint->authenticated ||
      std::strcmp(endpoint->participant_id, requested_owner_node_id) != 0 ||
      !ParticipantEndpointCache::is_online(endpoint, now_ms))
    return PairingSiteSyncDecision::OWNER_OFFLINE;

  const bool local_payload_ready =
      !configuration.floorplan.committed || local_floorplan_replica_ready;
  return local_payload_ready &&
                 participant_endpoint_matches_configuration(endpoint,
                                                            configuration)
             ? PairingSiteSyncDecision::ALREADY_CURRENT
             : PairingSiteSyncDecision::START_STRICT;
}

SiteReplicationRouteResult resolve_site_replication_route(
    const SiteDirectorySnapshot *directory,
    const SiteConfigurationSnapshot &configuration,
    const char *local_node_id, const char *requested_owner_node_id,
    SiteReplicationRoute *route) {
  if (local_node_id == nullptr || local_node_id[0] == '\0' ||
      route == nullptr)
    return SiteReplicationRouteResult::INVALID_ARGUMENT;
  *route = {};

  if (directory == nullptr || directory->compatibility_mirror) {
    const char *owner =
        requested_owner_node_id != nullptr &&
                requested_owner_node_id[0] != '\0'
            ? requested_owner_node_id
            : configuration.configuration_owner_node_id;
    if (owner == nullptr || owner[0] == '\0')
      return SiteReplicationRouteResult::ASSIGNMENT_MISSING;
    if (std::strcmp(owner, local_node_id) == 0)
      return SiteReplicationRouteResult::LOCAL_OWNER;
    copy_text(route->floorplan.owner_node_id,
              sizeof(route->floorplan.owner_node_id), owner);
    return SiteReplicationRouteResult::LEGACY_FALLBACK;
  }

  if (!valid_site_directory(*directory) ||
      std::strcmp(directory->site_id, configuration.site_id) != 0)
    return SiteReplicationRouteResult::INVALID_DIRECTORY;
  const auto *local_member = configuration.find(local_node_id);
  if (local_member == nullptr ||
      !local_member->spatial_profile.committed ||
      local_member->spatial_profile.group_id[0] == '\0')
    return SiteReplicationRouteResult::ASSIGNMENT_MISSING;
  const auto *group =
      directory->find_group(local_member->spatial_profile.group_id);
  if (group == nullptr)
    return SiteReplicationRouteResult::ASSIGNMENT_MISSING;
  const auto *floorplan = directory->find_floorplan(group->floorplan_id);
  if (floorplan == nullptr)
    return SiteReplicationRouteResult::ASSIGNMENT_MISSING;
  if (requested_owner_node_id != nullptr &&
      requested_owner_node_id[0] != '\0' &&
      std::strcmp(requested_owner_node_id, floorplan->owner_node_id) != 0)
    return SiteReplicationRouteResult::OWNER_MISMATCH;
  if (std::strcmp(floorplan->owner_node_id, local_node_id) == 0)
    return SiteReplicationRouteResult::LOCAL_OWNER;

  route->manifest_bound = true;
  route->floorplan = *floorplan;
  return SiteReplicationRouteResult::OK;
}

bool site_replication_route_matches_configuration(
    const SiteReplicationRoute &route,
    const SiteConfigurationSnapshot &configuration) {
  if (!route.manifest_bound)
    return true;
  if (std::strcmp(route.floorplan.owner_node_id,
                  configuration.configuration_owner_node_id) != 0 ||
      route.floorplan.committed != configuration.floorplan.committed)
    return false;
  if (!route.floorplan.committed)
    return true;
  return std::strcmp(route.floorplan.floorplan_id,
                     configuration.floorplan.floorplan_id) == 0 &&
         route.floorplan.geometry_version ==
             configuration.floorplan.geometry_version &&
         route.floorplan.config_hash == configuration.floorplan.config_hash &&
         route.floorplan.image_hash == configuration.floorplan.image_hash;
}

SiteConfigurationReplicationResult
validate_replicated_floorplan_configuration(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const SiteConfigurationSnapshot &remote,
    const SiteReplicationRoute &route) {
  if (!route.manifest_bound || identity.node_id[0] == '\0' ||
      identity.site_id[0] == '\0')
    return SiteConfigurationReplicationResult::INVALID_ARGUMENT;
  if (std::strcmp(identity.site_id, current.site_id) != 0 ||
      std::strcmp(identity.site_id, remote.site_id) != 0)
    return SiteConfigurationReplicationResult::SITE_MISMATCH;
  if (!remote.roster_committed ||
      remote.find(identity.node_id) == nullptr ||
      remote.find(identity.node_id)->membership !=
          ParticipantMembership::ACTIVE)
    return SiteConfigurationReplicationResult::LOCAL_MEMBER_MISSING;
  if (remote.generation != route.floorplan.owner_generation ||
      !site_replication_route_matches_configuration(route, remote))
    return SiteConfigurationReplicationResult::OWNER_MISMATCH;
  if (!same_site_topology(current, remote))
    return SiteConfigurationReplicationResult::REVISION_CONFLICT;
  if (current.generation == remote.generation &&
      site_replication_route_matches_configuration(route, current))
    return SiteConfigurationReplicationResult::ALREADY_CURRENT;
  return SiteConfigurationReplicationResult::OK;
}

SiteDirectoryReplicationValidation validate_replicated_site_directory(
    const char *site_id, const SiteDirectorySnapshot &current,
    const SiteDirectorySnapshot &remote, bool allow_authority_repair) {
  if (site_id == nullptr || site_id[0] == '\0' ||
      !valid_site_directory(current) || !valid_site_directory(remote) ||
      !remote.committed || remote.compatibility_mirror ||
      std::strcmp(remote.site_id, site_id) != 0 ||
      std::strcmp(current.site_id, site_id) != 0)
    return SiteDirectoryReplicationValidation::INVALID_DIRECTORY;
  // A compatibility mirror is derived from Site Configuration and its
  // topology revision is not a canonical Directory authority revision.
  if (current.compatibility_mirror)
    return SiteDirectoryReplicationValidation::OK;
  if (remote.topology_revision < current.topology_revision &&
      !allow_authority_repair)
    return SiteDirectoryReplicationValidation::STALE_REMOTE;
  if (remote.topology_revision == current.topology_revision) {
    if (same_site_directory_payload(current, remote))
      return SiteDirectoryReplicationValidation::ALREADY_CURRENT;
    return allow_authority_repair
               ? SiteDirectoryReplicationValidation::OK
               : SiteDirectoryReplicationValidation::REVISION_CONFLICT;
  }
  return SiteDirectoryReplicationValidation::OK;
}

bool site_replication_participant_authorized(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id) {
  if (participant_id == nullptr || participant_id[0] == '\0')
    return false;
  const auto *participant = participants.find(participant_id);
  if (participant != nullptr &&
      participant->membership == ParticipantMembership::REVOKED)
    return false;
  return site_configuration_member_active(configuration, participant_id) ||
         participant != nullptr;
}

uint8_t site_authority_rejoin_quorum_required(
    const SiteConfigurationSnapshot &configuration) {
  const std::size_t active = configuration.active_member_count();
  if (active <= 2)
    return 1;
  return static_cast<uint8_t>(active / 2U + 1U);
}

namespace {

bool endpoint_matches_authority_evidence(
    const ParticipantEndpoint *endpoint,
    const SiteAuthorityReplicationEvidence &evidence, uint32_t now_ms) {
  return ParticipantEndpointCache::is_online(endpoint, now_ms) &&
         endpoint->site_credentials_complete &&
         endpoint->site_configuration_authority_epoch ==
             evidence.authority_epoch &&
         endpoint->site_configuration_generation == evidence.generation &&
         endpoint->site_configuration_digest ==
             evidence.configuration_digest &&
         endpoint->site_topology_revision == evidence.topology_revision &&
         endpoint->site_topology_digest == evidence.topology_digest;
}

}  // namespace

bool site_authority_rejoin_evidence_matches(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache &endpoints,
    const SiteAuthorityReplicationEvidence &evidence, uint32_t now_ms) {
  const uint8_t required =
      site_authority_rejoin_quorum_required(configuration);
  if (!evidence.confirmed() || evidence.quorum_required != required)
    return false;
  const auto order = compare_site_configuration_revision(
      evidence.authority_epoch, evidence.generation,
      configuration.authority_epoch, configuration.generation);
  if (order == SiteConfigurationRevisionOrder::OLDER)
    return false;
  if (order == SiteConfigurationRevisionOrder::SAME &&
      (configuration.active_member_count() <= 2 ||
       evidence.configuration_digest ==
           site_configuration_digest(configuration) ||
       evidence.topology_revision != configuration.roster_revision ||
       evidence.topology_digest != site_topology_digest(configuration)))
    return false;

  uint8_t endorsements = 0;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        member.type != ParticipantType::NAMO_NODE)
      continue;
    if (endpoint_matches_authority_evidence(
            endpoints.find(member.participant_id), evidence, now_ms))
      ++endorsements;
  }
  return endorsements >= required;
}

bool site_configuration_replica_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *requester_participant_id) {
  if (configuration.site_id[0] == '\0' ||
      configuration.configuration_owner_node_id[0] == '\0' ||
      requester_participant_id == nullptr ||
      requester_participant_id[0] == '\0' ||
      std::strcmp(identity.site_id, configuration.site_id) != 0)
    return false;
  const auto *local = configuration.find(identity.node_id);
  const auto *requester = configuration.find(requester_participant_id);
  return local != nullptr && requester != nullptr &&
         local->membership == ParticipantMembership::ACTIVE &&
         local->type == ParticipantType::NAMO_NODE &&
         requester->membership != ParticipantMembership::REVOKED &&
         requester->type == ParticipantType::NAMO_NODE;
}

namespace {

bool authority_rejoin_payload_allowed(
    const SiteConfigurationSnapshot &current,
    const SiteConfigurationSnapshot &remote,
    const ParticipantEndpointCache *endpoints,
    const SiteAuthorityReplicationEvidence &evidence,
    const char *source_node_id, uint32_t now_ms) {
  if (endpoints == nullptr || source_node_id == nullptr ||
      source_node_id[0] == '\0' ||
      !site_authority_rejoin_evidence_matches(current, *endpoints, evidence,
                                               now_ms) ||
      remote.authority_epoch != evidence.authority_epoch ||
      remote.generation != evidence.generation ||
      site_configuration_digest(remote) != evidence.configuration_digest ||
      remote.roster_revision != evidence.topology_revision ||
      site_topology_digest(remote) != evidence.topology_digest)
    return false;
  const auto *source = endpoints->find(source_node_id);
  return endpoint_matches_authority_evidence(source, evidence, now_ms);
}

}  // namespace

bool site_directory_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site) {
  const auto *member = configuration.find(participant_id);
  if (!valid_site_directory(directory) || !directory.committed ||
      directory.compatibility_mirror || !site.local_leader ||
      std::strcmp(site.leader_node_id, identity.node_id) != 0 ||
      std::strcmp(directory.site_id, identity.site_id) != 0 ||
      member == nullptr || member->type != ParticipantType::NAMO_NODE ||
      member->membership != ParticipantMembership::ACTIVE)
    return false;
  (void) directory;
  return true;
}

bool site_directory_replication_required(
    const SiteCoordinatorState &site,
    const SiteConfigurationSnapshot &configuration,
    const char *local_participant_id) {
  const auto *member = configuration.find(local_participant_id);
  return !site.local_leader && site.leader_node_id[0] != '\0' &&
         member != nullptr && member->type == ParticipantType::NAMO_NODE &&
         member->membership == ParticipantMembership::ACTIVE;
}

namespace {

bool site_directory_bootstrap_candidate_ready(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials,
    const char *participant_id, uint32_t now_ms) {
  const auto *member = configuration.find(participant_id);
  const auto *endpoint = endpoints.find(participant_id);
  const auto *credential = credentials.find(participant_id);
  return member != nullptr && member->type == ParticipantType::NAMO_NODE &&
         member->membership == ParticipantMembership::ACTIVE &&
         credential != nullptr && credential->enabled &&
         ParticipantEndpointCache::is_online(endpoint, now_ms) &&
         participant_endpoint_matches_configuration(endpoint, configuration) &&
         endpoint->site_directory_revision != 0 &&
         endpoint->site_directory_digest != 0;
}

}  // namespace

const char *site_directory_bootstrap_source(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials, uint32_t now_ms) {
  const auto *local = configuration.find(identity.node_id);
  if (!configuration.roster_committed ||
      std::strcmp(configuration.site_id, identity.site_id) != 0 ||
      local == nullptr || local->type != ParticipantType::NAMO_NODE ||
      local->membership == ParticipantMembership::REVOKED ||
      !valid_site_directory(directory) ||
      !directory.compatibility_mirror ||
      std::strcmp(directory.site_id, identity.site_id) != 0)
    return nullptr;

  // Enrollment is initiated through the Configuration Owner and the joiner
  // has a direct credential for it before it can receive the full Site
  // credential bundle. Use that already-authenticated path for the initial
  // read-only Directory copy at every supported Site size. Role election is
  // deliberately not a prerequisite for enrollment bootstrap.
  const char *owner = configuration.configuration_owner_node_id;
  if (owner[0] != '\0' && std::strcmp(owner, identity.node_id) != 0 &&
      site_directory_bootstrap_candidate_ready(
          configuration, endpoints, credentials, owner, now_ms))
    return owner;

  if (site.leader_node_id[0] != '\0' &&
      std::strcmp(site.leader_node_id, identity.node_id) != 0) {
    return site_directory_bootstrap_candidate_ready(
               configuration, endpoints, credentials, site.leader_node_id,
               now_ms)
               ? site.leader_node_id
               : nullptr;
  }

  // With no elected Site Leader, only the bounded one- or two-member
  // bootstrap case may use a canonical peer directly. Larger Sites must let
  // the ordinary Directory quorum fence elect an authority first.
  if (configuration.active_member_count() > 2)
    return nullptr;

  const char *winner = nullptr;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, identity.node_id) == 0 ||
        !site_directory_bootstrap_candidate_ready(
            configuration, endpoints, credentials, member.participant_id,
            now_ms))
      continue;
    if (winner == nullptr || std::strcmp(member.participant_id, winner) < 0)
      winner = member.participant_id;
  }
  return winner;
}

bool site_directory_bootstrap_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints, uint32_t now_ms) {
  const auto *local = configuration.find(identity.node_id);
  const auto *requester = configuration.find(participant_id);
  if (!configuration.roster_committed ||
      local == nullptr || local->type != ParticipantType::NAMO_NODE ||
      local->membership != ParticipantMembership::ACTIVE ||
      !valid_site_directory(directory) || !directory.committed ||
      directory.compatibility_mirror ||
      std::strcmp(directory.site_id, identity.site_id) != 0 ||
      std::strcmp(configuration.site_id, identity.site_id) != 0 ||
      requester == nullptr || requester->type != ParticipantType::NAMO_NODE ||
      requester->membership == ParticipantMembership::REVOKED)
    return false;

  const bool local_site_leader =
      site.local_leader && site.leader_node_id[0] != '\0' &&
      std::strcmp(site.leader_node_id, identity.node_id) == 0;
  if (local_site_leader)
    return true;

  const bool local_owner =
      std::strcmp(configuration.configuration_owner_node_id,
                  identity.node_id) == 0;
  const uint8_t active_members = configuration.active_member_count();
  if (active_members <= 2)
    return local_owner ||
           std::strcmp(configuration.configuration_owner_node_id,
                       participant_id) == 0;
  if (!local_owner)
    return false;

  // In a larger established Site the Owner may serve enrollment bootstrap
  // without taking a leadership role, but only while the elected Site Leader
  // confirms the exact same canonical Directory under a live quorum.
  if (!site.configuration_ready || !site.quorum_available ||
      site.participants_expected != active_members ||
      site.leader_node_id[0] == '\0')
    return false;
  const auto *leader_endpoint = endpoints.find(site.leader_node_id);
  return ParticipantEndpointCache::is_online(leader_endpoint, now_ms) &&
         participant_endpoint_matches_configuration(leader_endpoint,
                                                     configuration) &&
         leader_endpoint->site_directory_revision ==
             directory.topology_revision &&
         leader_endpoint->site_directory_digest ==
             site_directory_payload_hash(directory);
}

bool site_directory_mirror_promotion_allowed(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials, uint32_t now_ms) {
  const auto *local = configuration.find(identity.node_id);
  const uint8_t active_members = configuration.active_member_count();
  if (!configuration.roster_committed || active_members <= 1 ||
      local == nullptr || local->type != ParticipantType::NAMO_NODE ||
      local->membership != ParticipantMembership::ACTIVE ||
      !valid_site_directory(directory) || !directory.committed ||
      !directory.compatibility_mirror ||
      std::strcmp(configuration.site_id, identity.site_id) != 0 ||
      std::strcmp(directory.site_id, identity.site_id) != 0 ||
      !site.configuration_ready || !site.quorum_available ||
      site.participants_expected != active_members || !site.local_leader ||
      std::strcmp(site.leader_node_id, identity.node_id) != 0)
    return false;

  uint8_t authenticated_mirror_members = 1;
  for (const auto &member : configuration.members) {
    if (!member.occupied || member.type != ParticipantType::NAMO_NODE ||
        member.membership != ParticipantMembership::ACTIVE ||
        std::strcmp(member.participant_id, identity.node_id) == 0)
      continue;
    const auto *endpoint = endpoints.find(member.participant_id);
    if (!ParticipantEndpointCache::is_online(endpoint, now_ms) ||
        !participant_endpoint_matches_configuration(endpoint, configuration))
      continue;

    // Any authenticated canonical advertisement wins over a local mirror.
    // The node must pull that Directory instead of minting a competing one.
    if (endpoint->site_directory_revision != 0 ||
        endpoint->site_directory_digest != 0)
      return false;

    const auto *credential = credentials.find(member.participant_id);
    if (credential != nullptr && credential->enabled)
      ++authenticated_mirror_members;
  }

  return authenticated_mirror_members >= strict_majority(active_members);
}

#ifdef USE_ESP32

SiteReplicationTransport::SiteReplicationTransport() = default;

SiteReplicationTransport::~SiteReplicationTransport() {
  this->destroy_socket_();
  this->destroy_workspace_();
}

bool SiteReplicationTransport::setup(
    RadarStorage *storage,
    ParticipantCredentialSnapshot *credential_snapshot,
    ParticipantCredentialStorageBackend *credential_backend,
    ParticipantCredentialStore *credential_store,
    const ParticipantEndpointCache *endpoints) {
  this->storage_ = storage;
  this->credential_snapshot_ = credential_snapshot;
  this->credentials_ = credential_snapshot != nullptr
                           ? &credential_snapshot->registry
                           : nullptr;
  this->credential_backend_ = credential_backend;
  this->credential_store_ = credential_store;
  this->endpoints_ = endpoints;
  const bool ready =
      storage != nullptr && this->credentials_ != nullptr &&
      credential_backend != nullptr && credential_store != nullptr &&
      endpoints != nullptr;
  if (!ready) {
    ESP_LOGE(TAG,
             "Replication setup failed stage=dependencies storage=%s "
             "credentials=%s credential_store=%s endpoints=%s",
             storage != nullptr ? "ready" : "missing",
             this->credentials_ != nullptr ? "ready" : "missing",
             credential_backend != nullptr && credential_store != nullptr
                 ? "ready"
                 : "missing",
             endpoints != nullptr ? "ready" : "missing");
    return false;
  }
  return this->allocate_workspace_();
}

SiteReplicationResult SiteReplicationTransport::start(
    const char *owner_node_id, uint32_t now_ms,
    SiteConfigurationReplicationMode mode,
    const SiteReplicationRoute *route,
    const SiteAuthorityReplicationEvidence *evidence) {
  if (owner_node_id == nullptr || owner_node_id[0] == '\0')
    return SiteReplicationResult::INVALID_OWNER;
  if (this->workspace_ == nullptr)
    return SiteReplicationResult::STORAGE_FAILED;
  if (site_replication_session_matches(this->diagnostics_, owner_node_id,
                                       mode))
    return SiteReplicationResult::OK;
  if (this->diagnostics_.state != SiteReplicationState::IDLE &&
      this->diagnostics_.state != SiteReplicationState::COMPLETE &&
      this->diagnostics_.state != SiteReplicationState::FAILED &&
      this->diagnostics_.state != SiteReplicationState::CONFLICT)
    return SiteReplicationResult::BUSY;
  if (this->credentials_ == nullptr ||
      this->credentials_->find(owner_node_id) == nullptr)
    return SiteReplicationResult::CREDENTIAL_MISSING;
  if (route != nullptr &&
      std::strcmp(route->floorplan.owner_node_id, owner_node_id) != 0)
    return SiteReplicationResult::INVALID_OWNER;
  if (mode == SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
      (evidence == nullptr || !evidence->confirmed()))
    return SiteReplicationResult::INVALID_OWNER;

  this->cancel();
  copy_text(this->diagnostics_.owner_node_id,
            sizeof(this->diagnostics_.owner_node_id), owner_node_id);
  this->diagnostics_.target = SiteReplicationTarget::SITE_CONFIGURATION;
  this->diagnostics_.state = SiteReplicationState::WAITING_FOR_OWNER;
  this->diagnostics_.result = SiteReplicationResult::IDLE;
  this->diagnostics_.validation_result =
      SiteConfigurationReplicationResult::NOT_EVALUATED;
  this->diagnostics_.mode = mode;
  this->request_id_ = now_ms == 0 ? 1 : now_ms;
  this->session_id_ = this->request_id_;
  this->session_started_ms_ = this->request_id_;
  this->replication_mode_ = mode;
  if (evidence != nullptr)
    this->authority_evidence_ = *evidence;
  if (route != nullptr)
    this->workspace_->route = *route;
  return SiteReplicationResult::OK;
}

SiteReplicationResult SiteReplicationTransport::start_directory(
    const char *source_node_id, uint32_t now_ms,
    SiteDirectoryReplicationMode mode) {
  if (source_node_id == nullptr || source_node_id[0] == '\0')
    return SiteReplicationResult::INVALID_OWNER;
  if (this->workspace_ == nullptr)
    return SiteReplicationResult::STORAGE_FAILED;
  if (this->diagnostics_.state != SiteReplicationState::IDLE &&
      this->diagnostics_.state != SiteReplicationState::COMPLETE &&
      this->diagnostics_.state != SiteReplicationState::FAILED &&
      this->diagnostics_.state != SiteReplicationState::CONFLICT)
    return SiteReplicationResult::BUSY;
  if (this->credentials_ == nullptr ||
      this->credentials_->find(source_node_id) == nullptr)
    return SiteReplicationResult::CREDENTIAL_MISSING;

  this->cancel();
  copy_text(this->diagnostics_.owner_node_id,
            sizeof(this->diagnostics_.owner_node_id), source_node_id);
  this->diagnostics_.target = SiteReplicationTarget::SITE_DIRECTORY;
  this->diagnostics_.state = SiteReplicationState::WAITING_FOR_OWNER;
  this->diagnostics_.result = SiteReplicationResult::IDLE;
  this->request_id_ = now_ms == 0 ? 1 : now_ms;
  this->session_started_ms_ = this->request_id_;
  this->directory_mode_ = mode;
  return SiteReplicationResult::OK;
}

SiteReplicationResult SiteReplicationTransport::start_credentials(
    const char *owner_node_id, uint32_t now_ms, bool authority_bootstrap) {
  if (owner_node_id == nullptr || owner_node_id[0] == '\0')
    return SiteReplicationResult::INVALID_OWNER;
  if (this->workspace_ == nullptr || this->credential_snapshot_ == nullptr ||
      this->credential_backend_ == nullptr || this->credential_store_ == nullptr)
    return SiteReplicationResult::STORAGE_FAILED;
  if (this->diagnostics_.state != SiteReplicationState::IDLE &&
      this->diagnostics_.state != SiteReplicationState::COMPLETE &&
      this->diagnostics_.state != SiteReplicationState::FAILED &&
      this->diagnostics_.state != SiteReplicationState::CONFLICT)
    return SiteReplicationResult::BUSY;
  if (this->credentials_ == nullptr ||
      this->credentials_->find(owner_node_id) == nullptr)
    return SiteReplicationResult::CREDENTIAL_MISSING;

  this->cancel();
  copy_text(this->diagnostics_.owner_node_id,
            sizeof(this->diagnostics_.owner_node_id), owner_node_id);
  this->diagnostics_.target = SiteReplicationTarget::CREDENTIAL_BUNDLE;
  this->diagnostics_.state = SiteReplicationState::WAITING_FOR_OWNER;
  this->diagnostics_.result = SiteReplicationResult::IDLE;
  uint32_t random_request = 0;
  if (!esphome::random_bytes(reinterpret_cast<uint8_t *>(&random_request),
                             sizeof(random_request)) ||
      random_request == 0)
    random_request = now_ms == 0 ? 1 : now_ms;
  this->request_id_ = random_request;
  this->session_id_ = random_request;
  this->session_started_ms_ = now_ms == 0 ? 1 : now_ms;
  this->credential_authority_bootstrap_ = authority_bootstrap;
  return SiteReplicationResult::OK;
}

void SiteReplicationTransport::cancel() {
  if (this->storage_ != nullptr)
    this->storage_->cancel_floorplan_replica();
  const bool socket_ready = this->diagnostics_.socket_ready;
  const uint32_t requests = this->diagnostics_.requests;
  const uint32_t responses = this->diagnostics_.responses;
  const uint32_t rejected = this->diagnostics_.rejected;
  this->diagnostics_ = {};
  this->diagnostics_.socket_ready = socket_ready;
  this->diagnostics_.requests = requests;
  this->diagnostics_.responses = responses;
  this->diagnostics_.rejected = rejected;
  if (this->workspace_ != nullptr) {
    this->workspace_->received_configuration = {};
    this->workspace_->remote_configuration = {};
    this->workspace_->remote_directory = {};
    this->workspace_->remote_credentials = {};
    this->workspace_->credential_merge_scratch = {};
    this->workspace_->route = {};
  }
  this->received_bytes_ = 0;
  this->expected_bytes_ = 0;
  this->expected_hash_ = 0;
  this->expected_request_id_ = 0;
  this->expected_request_key_version_ = 0;
  this->expected_request_participant_token_ = 0;
  this->expected_response_target_ =
      SiteReplicationTarget::SITE_CONFIGURATION;
  this->config_size_ = 0;
  this->image_size_ = 0;
  this->config_hash_ = 0;
  this->image_hash_ = 0;
  this->session_started_ms_ = 0;
  this->request_sent_ms_ = 0;
  this->retry_count_ = 0;
  this->credential_probe_index_ = 0;
  this->credential_session_key_version_ = 0;
  this->credential_session_participant_token_ = 0;
  this->credential_authority_bootstrap_ = false;
  this->directory_mode_ = SiteDirectoryReplicationMode::STANDARD;
  this->authority_evidence_ = {};
  this->awaiting_response_ = false;
  this->replica_started_ = false;
  this->replication_mode_ = SiteConfigurationReplicationMode::STRICT;
}

void SiteReplicationTransport::loop(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    SiteConfigurationSnapshot *configuration,
    SiteConfigurationStorageBackend *configuration_backend,
    SiteConfigurationStore *configuration_store,
    SiteDirectorySnapshot *directory,
    SiteDirectoryStorageBackend *directory_backend,
    SiteDirectoryStore *directory_store,
    const SiteCoordinatorState &site, uint32_t now_ms) {
  const auto &source = this->source_lease_.diagnostics();
  if (source.active && this->source_lease_.remaining_ms(now_ms) == 0) {
    ESP_LOGI(TAG, "Floorplan source lease expired participant=%s",
             source.participant_id);
    this->source_lease_.expire(now_ms);
  }
  if (this->storage_ == nullptr || this->credentials_ == nullptr ||
      this->endpoints_ == nullptr || this->workspace_ == nullptr ||
      configuration == nullptr || !network::is_connected() ||
      !this->ensure_socket_(now_ms))
    return;
  this->receive_(identity, participants, *configuration, directory, site,
                 now_ms);
  this->advance_(identity, participants, configuration, configuration_backend,
                 configuration_store, directory, directory_backend,
                 directory_store, site, now_ms);
}

bool SiteReplicationTransport::allocate_workspace_() {
  if (this->workspace_ != nullptr)
    return true;
#ifdef USE_PSRAM
  this->workspace_ = static_cast<SiteReplicationWorkspace *>(
      heap_caps_calloc(1, sizeof(SiteReplicationWorkspace),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->workspace_ != nullptr) {
    ESP_LOGI(TAG, "Replication workspace allocated in PSRAM size=%u",
             static_cast<unsigned>(sizeof(SiteReplicationWorkspace)));
    return true;
  }
  ESP_LOGE(
      TAG,
      "Replication setup failed stage=workspace size=%u psram_free=%u "
      "psram_largest=%u",
      static_cast<unsigned>(sizeof(SiteReplicationWorkspace)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#else
  ESP_LOGE(TAG,
           "Replication setup failed stage=workspace reason=psram_disabled "
           "size=%u",
           static_cast<unsigned>(sizeof(SiteReplicationWorkspace)));
#endif
  return false;
}

void SiteReplicationTransport::destroy_workspace_() {
#ifdef USE_PSRAM
  if (this->workspace_ != nullptr)
    heap_caps_free(this->workspace_);
#endif
  this->workspace_ = nullptr;
}

bool SiteReplicationTransport::ensure_socket_(uint32_t now_ms) {
  if (this->socket_ != nullptr)
    return true;
  if (this->next_socket_attempt_ms_ != 0 &&
      !time_reached(now_ms, this->next_socket_attempt_ms_))
    return false;
  this->next_socket_attempt_ms_ = now_ms + SOCKET_RETRY_MS;
  this->socket_ =
      socket::socket_ip_loop_monitored(SOCK_DGRAM, IPPROTO_UDP).release();
  if (this->socket_ == nullptr || this->socket_->setblocking(false) != 0) {
    ESP_LOGE(TAG,
             "Replication socket failed stage=create_or_nonblocking errno=%d",
             errno);
    this->destroy_socket_();
    return false;
  }
  sockaddr_storage server{};
  const socklen_t server_size = socket::set_sockaddr_any(
      reinterpret_cast<sockaddr *>(&server), sizeof(server), PORT);
  if (server_size == 0 ||
      this->socket_->bind(reinterpret_cast<sockaddr *>(&server), server_size) !=
          0) {
    ESP_LOGE(TAG, "Replication socket failed stage=bind port=%u errno=%d",
             static_cast<unsigned>(PORT), errno);
    this->destroy_socket_();
    return false;
  }
  this->next_socket_attempt_ms_ = 0;
  this->diagnostics_.socket_ready = true;
  ESP_LOGI(TAG, "Authenticated Site replication listening on UDP %u",
           static_cast<unsigned>(PORT));
  return true;
}

void SiteReplicationTransport::destroy_socket_() {
  delete this->socket_;
  this->socket_ = nullptr;
  this->diagnostics_.socket_ready = false;
}

void SiteReplicationTransport::receive_(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const SiteCoordinatorState &site, uint32_t now_ms) {
  if (!this->drain_pending_ && !this->socket_->ready())
    return;
  this->drain_pending_ = false;
  for (uint8_t index = 0; index < MAX_PACKETS_PER_LOOP; ++index) {
    sockaddr_storage sender{};
    socklen_t sender_size = sizeof(sender);
    const ssize_t received = this->socket_->recvfrom(
        this->packet_.data(), this->packet_.size(),
        reinterpret_cast<sockaddr *>(&sender), &sender_size);
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        this->diagnostics_.result = SiteReplicationResult::SOCKET_ERROR;
      return;
    }
    if (sender.ss_family != AF_INET) {
      this->diagnostics_.rejected++;
      continue;
    }
    SiteReplicationEnvelope envelope{};
    if (parse_site_replication_frame(
            this->packet_.data(), static_cast<std::size_t>(received),
            &envelope) != SiteReplicationCodecResult::OK) {
      this->diagnostics_.rejected++;
      continue;
    }
    const auto *credential = this->credentials_->resolve_provisioned(
        envelope.frame.participant_token, envelope.frame.key_version);
    std::array<uint8_t, SiteReplicationEnvelope::TAG_SIZE> expected{};
    if (credential == nullptr ||
        !hmac_sha256(credential->key, envelope.signed_data,
                     envelope.signed_size, expected.data()) ||
        !constant_time_equal(expected.data(), envelope.tag, expected.size())) {
      this->diagnostics_.rejected++;
      continue;
    }
    if (!site_replication_participant_authorized(
            participants, configuration, credential->participant_id)) {
      this->diagnostics_.rejected++;
      continue;
    }
    const auto *source = reinterpret_cast<const sockaddr_in *>(&sender);
    if (envelope.frame.kind == SiteReplicationKind::REQUEST) {
      this->serve_request_(envelope, *credential, source->sin_addr.s_addr,
                           ntohs(source->sin_port), identity, participants,
                           configuration, directory, site, now_ms);
    } else {
      this->accept_response_(envelope, *credential,
                             source->sin_addr.s_addr, configuration, directory,
                             site, now_ms);
    }
    if (index + 1 == MAX_PACKETS_PER_LOOP)
      this->drain_pending_ = true;
  }
}

void SiteReplicationTransport::serve_request_(
    const SiteReplicationEnvelope &envelope,
    const ParticipantCredentialRecord &credential, uint32_t source_ipv4,
    uint16_t source_port, const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const SiteCoordinatorState &site, uint32_t now_ms) {
  const auto *response_credential =
      envelope.frame.target == SiteReplicationTarget::CREDENTIAL_BUNDLE
          ? &credential
          : select_site_sender_credential(
                identity, &configuration, *this->credentials_,
                credential.participant_id);
  if (response_credential == nullptr || !response_credential->enabled) {
    ++this->diagnostics_.rejected;
    return;
  }
  SiteReplicationFrame response{};
  response.kind = SiteReplicationKind::RESPONSE;
  response.target = envelope.frame.target;
  response.key_version = response_credential->key_version;
  response.participant_token = response_credential->participant_token;
  response.request_id = envelope.frame.request_id;
  response.offset = envelope.frame.offset;
  response.site_generation =
      envelope.frame.target == SiteReplicationTarget::SITE_DIRECTORY
          ? (directory != nullptr ? directory->topology_revision : 0)
          : configuration.generation;
  bool source_lease_owned = false;

  const auto *member = configuration.find(credential.participant_id);
  auto &replay = this->workspace_->replay;
  const auto replay_admission = replay.classify(
      credential.participant_token, source_ipv4, envelope.frame, now_ms);

  if (!site_replication_participant_authorized(
          participants, configuration, credential.participant_id)) {
    response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
  } else if (replay_admission ==
             SiteReplicationReplayAdmission::DUPLICATE) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(source_port == 0 ? PORT : source_port);
    target.sin_addr.s_addr = source_ipv4;
    if (this->socket_->sendto(replay.packet(), replay.packet_size(), 0,
                              reinterpret_cast<sockaddr *>(&target),
                              sizeof(target)) >= 0) {
      ++this->diagnostics_.replayed;
    }
    return;
  } else if (replay_admission == SiteReplicationReplayAdmission::STALE) {
    ++this->diagnostics_.rejected;
    ++this->diagnostics_.stale_requests;
    return;
  } else if (envelope.frame.target ==
             SiteReplicationTarget::CREDENTIAL_BUNDLE) {
    uint32_t claim_high = 0;
    uint32_t claim_low = 0;
    const bool requester_authenticated =
        credential_request_claim(credential, credential.participant_id,
                                 &claim_high, &claim_low) &&
        envelope.frame.payload_hash == claim_high &&
        envelope.frame.site_generation == claim_low;
    const bool source_allowed = site_configuration_member_active(
        configuration, identity.node_id);
    if (!source_allowed ||
        !requester_authenticated ||
        !site_configuration_member_active(configuration,
                                          credential.participant_id)) {
      response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
      ESP_LOGW(
          TAG,
          "Credential bundle request rejected requester=%s reason=%s "
          "configured_owner=%s local_node=%s claim_valid=%s active_member=%s "
          "generation=%u roster=%u",
          credential.participant_id,
          !source_allowed
              ? "local_not_active_member"
              : !requester_authenticated
                    ? "requester_claim_mismatch"
                    : "requester_inactive",
          configuration.configuration_owner_node_id, identity.node_id,
          requester_authenticated ? "true" : "false",
          site_configuration_member_active(configuration,
                                           credential.participant_id)
              ? "true"
              : "false",
          static_cast<unsigned>(configuration.generation),
          static_cast<unsigned>(configuration.roster_revision));
    } else {
      const auto prepared = prepare_site_credential_bundle(
          identity, configuration, directory, site.leader_node_id,
          *this->credentials_, credential.participant_id,
          &this->workspace_->remote_credentials.registry);
      std::size_t encoded_size = 0;
      if (prepared != CredentialProvisioningResult::OK ||
          ParticipantCredentialCodec::encode_registry(
              this->workspace_->remote_credentials.registry,
              configuration.generation,
              this->workspace_->served_configuration.bytes.data(),
              this->workspace_->served_configuration.bytes.size(),
              &encoded_size) != ParticipantCredentialCodecResult::OK ||
          encoded_size == 0) {
        response.status =
            prepared == CredentialProvisioningResult::SOURCE_CREDENTIAL_MISSING
                ? SiteReplicationWireStatus::NOT_COMMITTED
                : SiteReplicationWireStatus::STORAGE_FAILED;
        ESP_LOGW(TAG,
                 "Credential bundle unavailable requester=%s result=%s",
                 credential.participant_id,
                 credential_provisioning_result_to_string(prepared));
      } else {
        response.total_size = static_cast<uint32_t>(encoded_size);
        response.payload_hash = fnv1a(
            this->workspace_->served_configuration.bytes.data(), encoded_size);
        response.site_generation = configuration.generation;
        if (response.offset > response.total_size) {
          response.status = SiteReplicationWireStatus::INVALID_RANGE;
        } else if (envelope.frame.length > 0) {
          response.length = std::min<uint32_t>(
              envelope.frame.length, response.total_size - response.offset);
          if (response.length > 0) {
            uint8_t *body = this->packet_.data() +
                            SiteReplicationEnvelope::PREFIX_SIZE;
            std::memcpy(
                body,
                this->workspace_->served_configuration.bytes.data() +
                    response.offset,
                response.length);
            if (!crypt_credential_chunk(
                    credential.key, credential.participant_token,
                    envelope.frame.request_id, response.offset, body,
                    response.length)) {
              response.status = SiteReplicationWireStatus::STORAGE_FAILED;
              response.length = 0;
            } else {
              response.body = body;
            }
          }
        }
      }
    }
  } else if (envelope.frame.target ==
             SiteReplicationTarget::SITE_DIRECTORY) {
    if (directory == nullptr ||
        (!site_directory_request_authorized(
             identity, configuration, credential.participant_id, *directory,
             site) &&
         (this->endpoints_ == nullptr ||
           !site_directory_bootstrap_request_authorized(
               identity, configuration, credential.participant_id, *directory,
               site, *this->endpoints_, now_ms)))) {
      response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
    } else {
      std::size_t encoded_size = 0;
      if (SiteDirectoryCodec::encode(
              *directory,
              this->workspace_->served_configuration.bytes.data(),
              this->workspace_->served_configuration.bytes.size(),
              &encoded_size) != SiteDirectoryCodecResult::OK ||
          encoded_size == 0) {
        response.status = SiteReplicationWireStatus::STORAGE_FAILED;
      } else {
        response.total_size = static_cast<uint32_t>(encoded_size);
        response.payload_hash =
            envelope.frame.length == 0
                ? site_directory_payload_hash(*directory)
                : fnv1a(
                      this->workspace_->served_configuration.bytes.data(),
                      encoded_size);
        if (response.payload_hash == 0)
          response.payload_hash = 1;
        if (response.offset > response.total_size) {
          response.status = SiteReplicationWireStatus::INVALID_RANGE;
        } else if (envelope.frame.length > 0) {
          response.length = std::min<uint32_t>(
              envelope.frame.length, response.total_size - response.offset);
          response.body =
              response.length > 0
                  ? this->workspace_->served_configuration.bytes.data() +
                        response.offset
                  : nullptr;
        }
      }
    }
  } else if (member == nullptr) {
    response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
  } else if (envelope.frame.target ==
                 SiteReplicationTarget::SITE_CONFIGURATION &&
             !site_configuration_replica_request_authorized(
                 identity, configuration, credential.participant_id)) {
    response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
  } else if (envelope.frame.target !=
                 SiteReplicationTarget::SITE_CONFIGURATION &&
             std::strcmp(configuration.configuration_owner_node_id,
                         identity.node_id) != 0) {
    response.status = SiteReplicationWireStatus::NOT_OWNER;
  } else {
    const auto admission = this->source_lease_.admit(
        credential.participant_id, source_ipv4, now_ms);
    if (admission == SiteReplicationSourceAdmission::BUSY) {
      response.status = SiteReplicationWireStatus::STORAGE_FAILED;
      response.payload_hash = SITE_REPLICATION_BUSY_PAYLOAD_HASH;
      const auto &source = this->source_lease_.diagnostics();
      ESP_LOGW(TAG,
               "Floorplan source busy requester=%s active=%s busy=%u",
               credential.participant_id, source.participant_id,
               static_cast<unsigned>(source.busy));
    } else if (admission ==
               SiteReplicationSourceAdmission::INVALID_ARGUMENT) {
      response.status = SiteReplicationWireStatus::PARTICIPANT_REJECTED;
    } else {
      source_lease_owned = true;
      if (admission == SiteReplicationSourceAdmission::ACQUIRED) {
        ESP_LOGI(TAG,
                 "Floorplan source lease acquired participant=%s lease=%ums",
                 credential.participant_id,
                 static_cast<unsigned>(
                     SiteReplicationSourceLease::LEASE_MS));
      }
      if (envelope.frame.target ==
          SiteReplicationTarget::SITE_CONFIGURATION) {
        std::size_t encoded_size = 0;
        if (SiteConfigurationCodec::encode(
                configuration,
                this->workspace_->served_configuration.bytes.data(),
                this->workspace_->served_configuration.bytes.size(),
                &encoded_size) !=
            SiteConfigurationCodecResult::OK) {
          response.status = SiteReplicationWireStatus::STORAGE_FAILED;
        } else {
          response.total_size = static_cast<uint32_t>(encoded_size);
          if (response.offset > response.total_size) {
            response.status = SiteReplicationWireStatus::INVALID_RANGE;
          } else {
            response.length = std::min<uint32_t>(
                envelope.frame.length,
                response.total_size - response.offset);
            response.body =
                response.length > 0
                    ? this->workspace_->served_configuration.bytes.data() +
                          response.offset
                    : nullptr;
          }
        }
      } else if (!configuration.floorplan.committed) {
        response.status = SiteReplicationWireStatus::NOT_COMMITTED;
      } else {
        const RadarPayloadTarget target =
            storage_target(envelope.frame.target);
        uint32_t payload_offset = 0;
        if (!this->storage_->payload_info(target, &payload_offset,
                                          &response.total_size)) {
          response.status = SiteReplicationWireStatus::NOT_FOUND;
        } else {
          const auto status = this->storage_->status();
          response.payload_hash =
              target == RadarPayloadTarget::CONFIG
                  ? status.header.config_hash
                  : status.header.image_hash;
          const uint32_t manifest_hash =
              target == RadarPayloadTarget::CONFIG
                  ? configuration.floorplan.config_hash
                  : configuration.floorplan.image_hash;
          if (response.payload_hash != manifest_hash) {
            response.status = SiteReplicationWireStatus::NOT_COMMITTED;
          } else if (response.offset > response.total_size) {
            response.status = SiteReplicationWireStatus::INVALID_RANGE;
          } else {
            response.length = std::min<uint32_t>(
                envelope.frame.length,
                response.total_size - response.offset);
            if (response.length > 0) {
              uint8_t *body =
                  this->packet_.data() +
                  SiteReplicationEnvelope::PREFIX_SIZE;
              if (!this->storage_->read_storage_range(
                      payload_offset + response.offset, body,
                      response.length)) {
                response.status =
                    SiteReplicationWireStatus::STORAGE_FAILED;
                response.length = 0;
              } else {
                response.body = body;
              }
            }
          }
        }
      }
    }
  }

  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  if (encode_site_replication_frame(
          response, this->packet_.data(), this->packet_.size(), &signed_size,
          &packet_size) != SiteReplicationCodecResult::OK ||
      !hmac_sha256(response_credential->key, this->packet_.data(), signed_size,
                   this->packet_.data() + signed_size))
    return;
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(source_port == 0 ? PORT : source_port);
  target.sin_addr.s_addr = source_ipv4;
  if (this->socket_->sendto(this->packet_.data(), packet_size, 0,
                            reinterpret_cast<sockaddr *>(&target),
                            sizeof(target)) < 0)
    return;

  replay.remember(credential.participant_token, source_ipv4, envelope.frame,
                  this->packet_.data(), packet_size, now_ms);

  const bool completed_floorplan =
      source_lease_owned &&
      envelope.frame.target == SiteReplicationTarget::FLOORPLAN_IMAGE &&
      response.status == SiteReplicationWireStatus::OK &&
      response.length > 0 &&
      response.offset + response.length == response.total_size;
  const bool completed_without_floorplan =
      source_lease_owned && !configuration.floorplan.committed &&
      envelope.frame.target == SiteReplicationTarget::SITE_CONFIGURATION &&
      response.status == SiteReplicationWireStatus::OK &&
      response.length > 0 &&
      response.offset + response.length == response.total_size;
  const bool terminal_error =
      source_lease_owned &&
      response.status != SiteReplicationWireStatus::OK;
  if (completed_floorplan || completed_without_floorplan || terminal_error) {
    const bool completed =
        completed_floorplan || completed_without_floorplan;
    if (this->source_lease_.release(credential.participant_id, source_ipv4,
                                    completed)) {
      ESP_LOGI(TAG,
               "Floorplan source lease released participant=%s result=%s",
               credential.participant_id,
               completed ? "complete" : "failed");
    }
  }
}

void SiteReplicationTransport::accept_response_(
    const SiteReplicationEnvelope &envelope,
    const ParticipantCredentialRecord &credential, uint32_t source_ipv4,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory, const SiteCoordinatorState &site,
    uint32_t now_ms) {
  const bool credential_response =
      this->expected_response_target_ ==
      SiteReplicationTarget::CREDENTIAL_BUNDLE;
  const auto *configuration_owner_endpoint = this->endpoints_->find(
      configuration.configuration_owner_node_id);
  const auto *response_endpoint =
      this->endpoints_->find(this->diagnostics_.owner_node_id);
  const bool handover_bootstrap_sender =
      !credential_response &&
      this->replication_mode_ ==
          SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER &&
      response_endpoint != nullptr &&
      automatic_handover_authority_available(
          configuration_owner_endpoint, now_ms,
          response_endpoint->site_configuration_authority_epoch,
          response_endpoint->site_configuration_generation,
          response_endpoint->site_configuration_digest,
          response_endpoint->site_topology_revision,
          response_endpoint->site_topology_digest) &&
      automatic_configuration_handover_bootstrap_allowed(
          configuration, credential.participant_id,
          this->diagnostics_.owner_node_id,
          response_endpoint->site_configuration_authority_epoch,
          response_endpoint->site_configuration_generation,
          response_endpoint->site_configuration_digest,
          response_endpoint->site_topology_revision,
          response_endpoint->site_topology_digest);
  const bool quorum_rejoin_sender =
      !credential_response &&
      this->replication_mode_ ==
          SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN &&
      response_endpoint != nullptr &&
      endpoint_matches_authority_evidence(
          response_endpoint, this->authority_evidence_, now_ms) &&
      site_authority_rejoin_evidence_matches(
          configuration, *this->endpoints_, this->authority_evidence_, now_ms);
  const bool expected_sender =
      credential_response
          ? credential.key_version == this->expected_request_key_version_ &&
                credential.participant_token ==
                    this->expected_request_participant_token_
          : std::strcmp(credential.participant_id,
                        this->diagnostics_.owner_node_id) == 0 ||
                handover_bootstrap_sender || quorum_rejoin_sender;
  if (!this->awaiting_response_ ||
      envelope.frame.request_id != this->expected_request_id_ ||
      !expected_sender)
    return;
  if (!site_replication_response_matches_request(
          envelope.frame, this->expected_request_id_,
          this->expected_response_target_)) {
    ++this->diagnostics_.rejected;
    ESP_LOGW(TAG,
             "Replication response ignored reason=target_mismatch "
             "request_id=%u expected=%s received=%s",
             static_cast<unsigned>(this->expected_request_id_),
             site_replication_target_to_string(
                 this->expected_response_target_),
             site_replication_target_to_string(envelope.frame.target));
    return;
  }
  const auto *endpoint = response_endpoint;
  if (endpoint == nullptr || endpoint->ipv4 != source_ipv4)
    return;
  const auto response_result = site_replication_response_result(
      envelope.frame.status, envelope.frame.payload_hash);
  if (response_result != SiteReplicationResult::OK) {
    if (credential_response &&
        this->diagnostics_.state ==
            SiteReplicationState::CREDENTIAL_METADATA &&
        envelope.frame.status ==
            SiteReplicationWireStatus::PARTICIPANT_REJECTED) {
      this->awaiting_response_ = false;
      this->expected_request_id_ = 0;
      this->expected_request_key_version_ = 0;
      this->expected_request_participant_token_ = 0;
      ++this->credential_probe_index_;
      ++this->diagnostics_.retries;
      const auto *next_probe = credential_probe_at(
          *this->credentials_, this->credential_probe_index_);
      ESP_LOGW(TAG,
               "Credential probe rejected owner=%s probe=%u next=%s "
               "request_id=%u",
               this->diagnostics_.owner_node_id,
               static_cast<unsigned>(this->credential_probe_index_),
               next_probe != nullptr ? "available" : "none",
               static_cast<unsigned>(envelope.frame.request_id));
      if (next_probe == nullptr)
        this->fail_(
            SiteReplicationResult::AUTHENTICATION_FAILED,
            SiteReplicationFailureDetail::CREDENTIAL_PROBE_REJECTED);
      return;
    }
    this->fail_(response_result);
    return;
  }

  this->diagnostics_.responses++;
  this->awaiting_response_ = false;
  this->retry_count_ = 0;
  this->request_sent_ms_ = now_ms;
  if (this->diagnostics_.state ==
      SiteReplicationState::CREDENTIAL_METADATA) {
    if (envelope.frame.offset != 0 || envelope.frame.length != 0 ||
        envelope.frame.total_size == 0 ||
        envelope.frame.total_size > ParticipantCredentialCodec::MAX_ENCODED_SIZE ||
        envelope.frame.payload_hash == 0 ||
        envelope.frame.site_generation == 0) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    this->credential_session_key_version_ =
        this->expected_request_key_version_;
    this->credential_session_participant_token_ =
        this->expected_request_participant_token_;
    this->diagnostics_.remote_generation = envelope.frame.site_generation;
    this->received_bytes_ = 0;
    this->expected_bytes_ = envelope.frame.total_size;
    this->expected_hash_ = envelope.frame.payload_hash;
    this->diagnostics_.total_bytes = envelope.frame.total_size;
    this->diagnostics_.transferred_bytes = 0;
    this->diagnostics_.state = SiteReplicationState::CREDENTIAL_PAYLOAD;
    return;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::CREDENTIAL_PAYLOAD) {
    if (envelope.frame.offset != this->received_bytes_ ||
        envelope.frame.total_size != this->expected_bytes_ ||
        envelope.frame.payload_hash != this->expected_hash_ ||
        envelope.frame.site_generation !=
            this->diagnostics_.remote_generation ||
        envelope.frame.length == 0 ||
        envelope.frame.length >
            this->expected_bytes_ - this->received_bytes_) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    auto *body = const_cast<uint8_t *>(envelope.frame.body);
    if (!crypt_credential_chunk(
            credential.key, credential.participant_token,
            envelope.frame.request_id, envelope.frame.offset, body,
            envelope.frame.length)) {
      this->fail_(
          SiteReplicationResult::AUTHENTICATION_FAILED,
          SiteReplicationFailureDetail::CREDENTIAL_PAYLOAD_CRYPTO_FAILED);
      return;
    }
    std::memcpy(this->workspace_->received_configuration.bytes.data() +
                    this->received_bytes_,
                body, envelope.frame.length);
    this->received_bytes_ += envelope.frame.length;
    this->diagnostics_.transferred_bytes += envelope.frame.length;
    if (this->received_bytes_ == this->expected_bytes_) {
      if (fnv1a(this->workspace_->received_configuration.bytes.data(),
                this->expected_bytes_) != this->expected_hash_ ||
          ParticipantCredentialCodec::decode(
              this->workspace_->received_configuration.bytes.data(),
              this->expected_bytes_,
              &this->workspace_->remote_credentials) !=
              ParticipantCredentialCodecResult::OK ||
          this->workspace_->remote_credentials.generation !=
              this->diagnostics_.remote_generation) {
        this->fail_(SiteReplicationResult::INVALID_CONFIGURATION);
        return;
      }
      this->diagnostics_.state =
          SiteReplicationState::CREDENTIAL_COMMITTING;
    }
    return;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::DIRECTORY_METADATA) {
    if (directory == nullptr || envelope.frame.offset != 0 ||
        envelope.frame.length != 0 ||
        envelope.frame.total_size == 0 ||
        envelope.frame.total_size >
            this->workspace_->received_configuration.bytes.size() ||
        envelope.frame.payload_hash == 0 ||
        envelope.frame.site_generation == 0) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    this->diagnostics_.remote_generation =
        envelope.frame.site_generation;
    const bool canonical_site_leader =
        !site.local_leader && site.leader_node_id[0] != '\0' &&
        std::strcmp(site.leader_node_id,
                    this->diagnostics_.owner_node_id) == 0;
    if (envelope.frame.site_generation < directory->topology_revision &&
        !canonical_site_leader) {
      this->diagnostics_.state = SiteReplicationState::CONFLICT;
      this->diagnostics_.result = SiteReplicationResult::REVISION_CONFLICT;
      return;
    }
    if (envelope.frame.site_generation == directory->topology_revision) {
      if (site_directory_payload_hash(*directory) ==
          envelope.frame.payload_hash) {
        this->diagnostics_.state = SiteReplicationState::COMPLETE;
        this->diagnostics_.result = SiteReplicationResult::OK;
        return;
      }
      if (!directory->compatibility_mirror && !canonical_site_leader) {
        this->diagnostics_.state = SiteReplicationState::CONFLICT;
        this->diagnostics_.result =
            SiteReplicationResult::REVISION_CONFLICT;
        return;
      }
    }
    this->received_bytes_ = 0;
    this->expected_bytes_ = envelope.frame.total_size;
    this->expected_hash_ = 0;
    this->diagnostics_.total_bytes = envelope.frame.total_size;
    this->diagnostics_.transferred_bytes = 0;
    this->diagnostics_.state = SiteReplicationState::DIRECTORY_PAYLOAD;
    return;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::DIRECTORY_PAYLOAD) {
    if (envelope.frame.offset != this->received_bytes_ ||
        envelope.frame.total_size != this->expected_bytes_ ||
        envelope.frame.site_generation !=
            this->diagnostics_.remote_generation ||
        envelope.frame.payload_hash == 0 ||
        (this->expected_hash_ != 0 &&
         envelope.frame.payload_hash != this->expected_hash_) ||
        envelope.frame.length == 0 ||
        envelope.frame.length >
            this->expected_bytes_ - this->received_bytes_) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    if (this->expected_hash_ == 0)
      this->expected_hash_ = envelope.frame.payload_hash;
    std::memcpy(this->workspace_->received_configuration.bytes.data() +
                    this->received_bytes_,
                envelope.frame.body, envelope.frame.length);
    this->received_bytes_ += envelope.frame.length;
    this->diagnostics_.transferred_bytes += envelope.frame.length;
    if (this->received_bytes_ == this->expected_bytes_) {
      if (fnv1a(this->workspace_->received_configuration.bytes.data(),
                this->expected_bytes_) != this->expected_hash_ ||
          SiteDirectoryCodec::decode(
              this->workspace_->received_configuration.bytes.data(),
              this->expected_bytes_,
              &this->workspace_->remote_directory) !=
              SiteDirectoryCodecResult::OK ||
          this->workspace_->remote_directory.topology_revision !=
              this->diagnostics_.remote_generation) {
        this->fail_(SiteReplicationResult::INVALID_CONFIGURATION);
        return;
      }
      this->diagnostics_.state =
          SiteReplicationState::DIRECTORY_COMMITTING;
    }
    return;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::SITE_CONFIGURATION) {
    if (envelope.frame.offset != this->received_bytes_ ||
        envelope.frame.total_size == 0 ||
        envelope.frame.total_size >
            this->workspace_->received_configuration.bytes.size() ||
        envelope.frame.length > envelope.frame.total_size -
                                    envelope.frame.offset) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    if (this->received_bytes_ == 0)
      this->expected_bytes_ = envelope.frame.total_size;
    if (this->expected_bytes_ != envelope.frame.total_size ||
        envelope.frame.length == 0) {
      this->fail_(SiteReplicationResult::INVALID_PACKET);
      return;
    }
    std::memcpy(this->workspace_->received_configuration.bytes.data() +
                    this->received_bytes_,
                envelope.frame.body, envelope.frame.length);
    this->received_bytes_ += envelope.frame.length;
    this->diagnostics_.transferred_bytes += envelope.frame.length;
    if (this->received_bytes_ == this->expected_bytes_) {
      if (SiteConfigurationCodec::decode(
              this->workspace_->received_configuration.bytes.data(),
              this->expected_bytes_,
              &this->workspace_->remote_configuration) !=
          SiteConfigurationCodecResult::OK) {
        this->fail_(SiteReplicationResult::INVALID_CONFIGURATION);
        return;
      }
      this->diagnostics_.remote_generation =
          this->workspace_->remote_configuration.generation;
    }
    return;
  }

  if (this->diagnostics_.state == SiteReplicationState::CONFIG_METADATA ||
      this->diagnostics_.state == SiteReplicationState::IMAGE_METADATA) {
    if (envelope.frame.length != 0 || envelope.frame.total_size == 0 ||
        envelope.frame.payload_hash == 0) {
      this->fail_(SiteReplicationResult::PAYLOAD_MISMATCH);
      return;
    }
    if (this->diagnostics_.state == SiteReplicationState::CONFIG_METADATA) {
      this->config_size_ = envelope.frame.total_size;
      this->config_hash_ = envelope.frame.payload_hash;
      if (this->config_hash_ !=
          this->workspace_->remote_configuration.floorplan.config_hash) {
        this->fail_(SiteReplicationResult::PAYLOAD_MISMATCH);
        return;
      }
      this->diagnostics_.state = SiteReplicationState::IMAGE_METADATA;
    } else {
      this->image_size_ = envelope.frame.total_size;
      this->image_hash_ = envelope.frame.payload_hash;
      if (this->image_hash_ !=
              this->workspace_->remote_configuration.floorplan.image_hash ||
          !this->storage_->start_floorplan_replica(
              this->config_size_, this->image_size_, this->session_id_)) {
        this->fail_(this->image_hash_ !=
                            this->workspace_->remote_configuration.floorplan.image_hash
                        ? SiteReplicationResult::PAYLOAD_MISMATCH
                        : SiteReplicationResult::STORAGE_FAILED);
        return;
      }
      this->replica_started_ = true;
      this->received_bytes_ = 0;
      this->expected_bytes_ = this->config_size_;
      this->diagnostics_.total_bytes =
          this->config_size_ + this->image_size_;
      this->diagnostics_.transferred_bytes = 0;
      this->diagnostics_.state = SiteReplicationState::CONFIG_PAYLOAD;
    }
    return;
  }

  if (this->diagnostics_.state == SiteReplicationState::CONFIG_PAYLOAD ||
      this->diagnostics_.state == SiteReplicationState::IMAGE_PAYLOAD) {
    const bool config =
        this->diagnostics_.state == SiteReplicationState::CONFIG_PAYLOAD;
    if (envelope.frame.offset != this->received_bytes_ ||
        envelope.frame.total_size != this->expected_bytes_ ||
        envelope.frame.length == 0 ||
        envelope.frame.length > this->expected_bytes_ - this->received_bytes_ ||
        !this->storage_->write_floorplan_replica_chunk(
            config ? RadarPayloadTarget::CONFIG : RadarPayloadTarget::IMAGE,
            this->received_bytes_, envelope.frame.body,
            envelope.frame.length, this->session_id_)) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    this->received_bytes_ += envelope.frame.length;
    this->diagnostics_.transferred_bytes += envelope.frame.length;
    if (this->received_bytes_ == this->expected_bytes_) {
      this->received_bytes_ = 0;
      if (config) {
        this->expected_bytes_ = this->image_size_;
        this->diagnostics_.state = SiteReplicationState::IMAGE_PAYLOAD;
      } else {
        this->diagnostics_.state = SiteReplicationState::COMMITTING;
      }
    }
  }
}

void SiteReplicationTransport::advance_(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    SiteConfigurationSnapshot *configuration,
    SiteConfigurationStorageBackend *configuration_backend,
    SiteConfigurationStore *configuration_store,
    SiteDirectorySnapshot *directory,
    SiteDirectoryStorageBackend *directory_backend,
    SiteDirectoryStore *directory_store,
    const SiteCoordinatorState &site, uint32_t now_ms) {
  if (this->diagnostics_.state == SiteReplicationState::IDLE ||
      this->diagnostics_.state == SiteReplicationState::COMPLETE ||
      this->diagnostics_.state == SiteReplicationState::FAILED ||
      this->diagnostics_.state == SiteReplicationState::CONFLICT)
    return;
  if (this->diagnostics_.target ==
      SiteReplicationTarget::CREDENTIAL_BUNDLE) {
    const auto *source_endpoint =
        this->endpoints_->find(this->diagnostics_.owner_node_id);
    const auto *source_credential =
        this->credentials_->find(this->diagnostics_.owner_node_id);
    const bool exact_source_ready =
        configuration != nullptr &&
        credential_provisioning_source_readiness(
            source_endpoint, *configuration, now_ms) ==
            CredentialProvisioningSourceReadiness::READY;
    const bool authority_bootstrap_ready =
        configuration != nullptr && this->credential_authority_bootstrap_ &&
        credential_provisioning_authority_bootstrap_source_ready(
            source_endpoint, *configuration,
            this->diagnostics_.owner_node_id);
    if (configuration == nullptr || source_credential == nullptr ||
        !source_credential->enabled ||
        !site_configuration_member_active(
            *configuration, this->diagnostics_.owner_node_id) ||
        (!exact_source_ready && !authority_bootstrap_ready)) {
      this->fail_(SiteReplicationResult::INVALID_OWNER);
      return;
    }
  }
  if (this->diagnostics_.target == SiteReplicationTarget::SITE_DIRECTORY) {
    const char *bootstrap_source =
        directory != nullptr && configuration != nullptr &&
                this->endpoints_ != nullptr && this->credentials_ != nullptr
            ? site_directory_bootstrap_source(
                  identity, *configuration, *directory, site,
                  *this->endpoints_, *this->credentials_, now_ms)
            : nullptr;
    bool source_valid = false;
    if (this->directory_mode_ ==
        SiteDirectoryReplicationMode::INITIAL_BOOTSTRAP) {
      source_valid = bootstrap_source != nullptr &&
                     std::strcmp(bootstrap_source,
                                 this->diagnostics_.owner_node_id) == 0;
    } else {
      source_valid = directory != nullptr && site.leader_node_id[0] != '\0' &&
                     std::strcmp(site.leader_node_id,
                                 this->diagnostics_.owner_node_id) == 0;
    }
    if (!source_valid) {
      this->fail_(SiteReplicationResult::INVALID_OWNER);
      return;
    }
  }
  if (this->awaiting_response_) {
    if (!time_reached(now_ms, this->request_sent_ms_ + REQUEST_TIMEOUT_MS))
      return;
    this->awaiting_response_ = false;
    if (this->diagnostics_.state ==
            SiteReplicationState::CREDENTIAL_METADATA &&
        this->diagnostics_.target ==
            SiteReplicationTarget::CREDENTIAL_BUNDLE) {
      ++this->credential_probe_index_;
      ++this->diagnostics_.retries;
      const auto *next_probe = credential_probe_at(
          *this->credentials_, this->credential_probe_index_);
      ESP_LOGW(TAG,
               "Credential probe timed out owner=%s probe=%u next=%s "
               "request_id=%u timeout_ms=%u",
               this->diagnostics_.owner_node_id,
               static_cast<unsigned>(this->credential_probe_index_),
               next_probe != nullptr ? "available" : "none",
               static_cast<unsigned>(this->expected_request_id_),
               static_cast<unsigned>(REQUEST_TIMEOUT_MS));
      if (next_probe == nullptr) {
        this->fail_(SiteReplicationResult::AUTHENTICATION_FAILED,
                    SiteReplicationFailureDetail::CREDENTIAL_PROBE_TIMEOUT);
        return;
      }
    } else if (++this->retry_count_ > MAX_RETRIES) {
      this->fail_(SiteReplicationResult::TIMEOUT);
      return;
    } else {
      this->diagnostics_.retries++;
    }
  }

  if (this->diagnostics_.state == SiteReplicationState::WAITING_FOR_OWNER) {
    const auto *endpoint =
        this->endpoints_->find(this->diagnostics_.owner_node_id);
    const bool credential_discovery_route =
        this->diagnostics_.target ==
            SiteReplicationTarget::CREDENTIAL_BUNDLE &&
        this->credential_authority_bootstrap_ && endpoint != nullptr &&
        endpoint->ipv4 != 0;
    if (!credential_discovery_route &&
        !ParticipantEndpointCache::is_online(endpoint, now_ms)) {
      this->diagnostics_.result = SiteReplicationResult::OWNER_OFFLINE;
      if (this->session_started_ms_ != 0 &&
          time_reached(now_ms, this->session_started_ms_ +
                                   OWNER_DISCOVERY_TIMEOUT_MS))
        this->fail_(SiteReplicationResult::OWNER_OFFLINE);
      return;
    }
    this->diagnostics_.result = SiteReplicationResult::IDLE;
    this->diagnostics_.state =
        this->diagnostics_.target == SiteReplicationTarget::SITE_DIRECTORY
            ? SiteReplicationState::DIRECTORY_METADATA
            : (this->diagnostics_.target ==
                       SiteReplicationTarget::CREDENTIAL_BUNDLE
                   ? SiteReplicationState::CREDENTIAL_METADATA
                   : SiteReplicationState::SITE_CONFIGURATION);
    this->received_bytes_ = 0;
    this->expected_bytes_ = 0;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::CREDENTIAL_COMMITTING) {
    const auto *request_credential =
        this->credentials_->resolve_provisioned(
            this->credential_session_participant_token_,
            this->credential_session_key_version_);
    if (this->diagnostics_.remote_generation != configuration->generation) {
      this->fail_(SiteReplicationResult::REVISION_CONFLICT);
      return;
    }
    if (this->credential_snapshot_ == nullptr ||
        this->credential_backend_ == nullptr ||
        this->credential_store_ == nullptr) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    if (request_credential == nullptr ||
        !credential_bundle_binds_requester(
             this->workspace_->remote_credentials.registry,
             configuration->configuration_owner_node_id,
             *request_credential)) {
      this->fail_(
          SiteReplicationResult::AUTHENTICATION_FAILED,
          SiteReplicationFailureDetail::CREDENTIAL_BUNDLE_BINDING_MISMATCH);
      return;
    }
    std::size_t backup_size = 0;
    if (ParticipantCredentialCodec::encode(
            *this->credential_snapshot_,
            this->workspace_->received_configuration.bytes.data(),
            this->workspace_->received_configuration.bytes.size(),
            &backup_size) != ParticipantCredentialCodecResult::OK) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    bool changed = false;
    const auto merged = merge_site_credential_bundle(
        identity, *configuration, directory, site.leader_node_id,
        this->workspace_->remote_credentials.registry,
        &this->credential_snapshot_->registry,
        &this->workspace_->credential_merge_scratch, &changed);
    if (merged != CredentialProvisioningResult::OK &&
        merged != CredentialProvisioningResult::ALREADY_COMPLETE) {
      ParticipantCredentialCodec::decode(
          this->workspace_->received_configuration.bytes.data(), backup_size,
          this->credential_snapshot_);
      ESP_LOGW(TAG, "Credential bundle rejected source=%s result=%s",
               this->diagnostics_.owner_node_id,
               credential_provisioning_result_to_string(merged));
      this->fail_(SiteReplicationResult::INVALID_CONFIGURATION);
      return;
    }
    if (changed &&
        this->credential_store_->update(
            *this->credential_backend_, this->credential_snapshot_->registry,
            this->credential_snapshot_) !=
            ParticipantCredentialStoreResult::UPDATED) {
      ParticipantCredentialCodec::decode(
          this->workspace_->received_configuration.bytes.data(), backup_size,
          this->credential_snapshot_);
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    ESP_LOGI(TAG,
             "Credential bundle applied source=%s generation=%u records=%u "
             "changed=%s",
             this->diagnostics_.owner_node_id,
             static_cast<unsigned>(configuration->generation),
             static_cast<unsigned>(
                 this->workspace_->remote_credentials.registry.size()),
             changed ? "true" : "false");
    this->diagnostics_.state = SiteReplicationState::COMPLETE;
    this->diagnostics_.result = SiteReplicationResult::OK;
    return;
  }

  if (this->diagnostics_.state ==
      SiteReplicationState::DIRECTORY_COMMITTING) {
    const bool canonical_site_leader =
        this->directory_mode_ == SiteDirectoryReplicationMode::STANDARD &&
        !site.local_leader && site.leader_node_id[0] != '\0' &&
        std::strcmp(site.leader_node_id,
                    this->diagnostics_.owner_node_id) == 0;
    const auto validation = validate_replicated_site_directory(
        identity.site_id, *directory, this->workspace_->remote_directory,
        canonical_site_leader);
    if (validation == SiteDirectoryReplicationValidation::ALREADY_CURRENT) {
      this->diagnostics_.state = SiteReplicationState::COMPLETE;
      this->diagnostics_.result = SiteReplicationResult::OK;
      return;
    }
    if (validation != SiteDirectoryReplicationValidation::OK) {
      this->diagnostics_.state = SiteReplicationState::CONFLICT;
      this->diagnostics_.result =
          validation ==
                  SiteDirectoryReplicationValidation::REVISION_CONFLICT ||
              validation ==
                  SiteDirectoryReplicationValidation::STALE_REMOTE
              ? SiteReplicationResult::REVISION_CONFLICT
              : SiteReplicationResult::INVALID_CONFIGURATION;
      return;
    }
    if (!this->persist_remote_directory_(directory, directory_backend,
                                         directory_store)) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    ESP_LOGI(
        TAG,
        "Site directory replicated source=%s topology=%u floorplans=%u groups=%u",
        this->diagnostics_.owner_node_id,
        static_cast<unsigned>(directory->topology_revision),
        static_cast<unsigned>(directory->floorplan_count()),
        static_cast<unsigned>(directory->group_count()));
    this->diagnostics_.state = SiteReplicationState::COMPLETE;
    this->diagnostics_.result = SiteReplicationResult::OK;
    return;
  }

  if (this->diagnostics_.state == SiteReplicationState::SITE_CONFIGURATION &&
      this->expected_bytes_ > 0 &&
      this->received_bytes_ == this->expected_bytes_) {
    if (!site_replication_route_matches_configuration(
            this->workspace_->route,
            this->workspace_->remote_configuration)) {
      this->diagnostics_.state = SiteReplicationState::CONFLICT;
      this->diagnostics_.result = SiteReplicationResult::PAYLOAD_MISMATCH;
      return;
    }
    const bool enrollment_source_valid =
        this->replication_mode_ !=
            SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT ||
        authenticated_enrollment_source_allowed(
            identity, *configuration, participants, *this->credentials_,
            this->diagnostics_.owner_node_id);
    const bool authority_rejoin_source_valid =
        this->replication_mode_ !=
            SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN ||
        authority_rejoin_payload_allowed(
            *configuration, this->workspace_->remote_configuration,
            this->endpoints_, this->authority_evidence_,
            this->diagnostics_.owner_node_id, now_ms);
    const auto validation =
        !enrollment_source_valid || !authority_rejoin_source_valid
        ? SiteConfigurationReplicationResult::OWNER_MISMATCH
        : this->workspace_->route.manifest_bound
        ? validate_replicated_floorplan_configuration(
              identity, *configuration,
              this->workspace_->remote_configuration,
              this->workspace_->route)
        : validate_replicated_site_configuration(
              identity, *configuration,
              this->workspace_->remote_configuration,
              this->replication_mode_, this->diagnostics_.owner_node_id);
    this->diagnostics_.validation_result = validation;
    if (validation != SiteConfigurationReplicationResult::OK &&
        validation != SiteConfigurationReplicationResult::ALREADY_CURRENT) {
      ESP_LOGW(TAG,
               "Site configuration rejected source=%s mode=%s validation=%s "
               "local_epoch=%u local_generation=%u remote_epoch=%u "
               "remote_generation=%u",
               this->diagnostics_.owner_node_id,
               site_configuration_replication_mode_to_string(
                   this->replication_mode_),
               site_configuration_replication_result_to_string(validation),
               static_cast<unsigned>(configuration->authority_epoch),
               static_cast<unsigned>(configuration->generation),
               static_cast<unsigned>(
                   this->workspace_->remote_configuration.authority_epoch),
               static_cast<unsigned>(
                   this->workspace_->remote_configuration.generation));
      this->diagnostics_.state = SiteReplicationState::CONFLICT;
      this->diagnostics_.result =
          validation == SiteConfigurationReplicationResult::REVISION_CONFLICT
              ? SiteReplicationResult::REVISION_CONFLICT
              : SiteReplicationResult::INVALID_CONFIGURATION;
      return;
    }
    if (!this->workspace_->remote_configuration.floorplan.committed) {
      this->diagnostics_.state = SiteReplicationState::COMMITTING;
    } else {
      const auto status = this->storage_->status();
      if (status.header.config_hash ==
              this->workspace_->remote_configuration.floorplan.config_hash &&
          status.header.image_hash ==
              this->workspace_->remote_configuration.floorplan.image_hash) {
        this->diagnostics_.state = SiteReplicationState::COMMITTING;
      } else {
        this->diagnostics_.state = SiteReplicationState::CONFIG_METADATA;
      }
    }
  }

  if (this->diagnostics_.state == SiteReplicationState::COMMITTING) {
    if (this->replica_started_ &&
        !this->storage_->commit_floorplan_replica(
            this->session_id_, this->config_hash_, this->image_hash_)) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    this->replica_started_ = false;
    if (!this->workspace_->remote_configuration.floorplan.committed &&
        !this->storage_->delete_floorplan()) {
      this->fail_(SiteReplicationResult::STORAGE_FAILED);
      return;
    }
    if (!this->persist_remote_configuration_(
            identity, participants, configuration, configuration_backend,
            configuration_store, now_ms)) {
      const auto final_validation =
          this->diagnostics_.validation_result;
      if (final_validation != SiteConfigurationReplicationResult::OK &&
          final_validation !=
              SiteConfigurationReplicationResult::ALREADY_CURRENT) {
        ESP_LOGW(TAG,
                 "Site configuration changed before commit source=%s "
                 "mode=%s validation=%s",
                 this->diagnostics_.owner_node_id,
                 site_configuration_replication_mode_to_string(
                     this->replication_mode_),
                 site_configuration_replication_result_to_string(
                     final_validation));
        this->diagnostics_.state = SiteReplicationState::CONFLICT;
        this->diagnostics_.result =
            final_validation ==
                    SiteConfigurationReplicationResult::REVISION_CONFLICT
                ? SiteReplicationResult::REVISION_CONFLICT
                : SiteReplicationResult::INVALID_CONFIGURATION;
      } else {
        this->fail_(SiteReplicationResult::STORAGE_FAILED);
      }
      return;
    }
    this->diagnostics_.state = SiteReplicationState::COMPLETE;
    this->diagnostics_.result = SiteReplicationResult::OK;
    return;
  }

  switch (this->diagnostics_.state) {
    case SiteReplicationState::DIRECTORY_METADATA:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::SITE_DIRECTORY, 0, 0,
                          now_ms);
      break;
    case SiteReplicationState::CREDENTIAL_METADATA:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::CREDENTIAL_BUNDLE, 0, 0,
                          now_ms);
      break;
    case SiteReplicationState::CREDENTIAL_PAYLOAD:
      this->send_request_(
          identity, configuration, SiteReplicationTarget::CREDENTIAL_BUNDLE,
          this->received_bytes_,
          std::min<uint32_t>(
              SiteReplicationEnvelope::MAX_CHUNK_SIZE,
              this->expected_bytes_ - this->received_bytes_),
          now_ms);
      break;
    case SiteReplicationState::DIRECTORY_PAYLOAD:
      this->send_request_(
          identity, configuration, SiteReplicationTarget::SITE_DIRECTORY,
          this->received_bytes_,
          std::min<uint32_t>(
              SiteReplicationEnvelope::MAX_CHUNK_SIZE,
              this->expected_bytes_ - this->received_bytes_),
          now_ms);
      break;
    case SiteReplicationState::SITE_CONFIGURATION:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::SITE_CONFIGURATION,
                          this->received_bytes_,
                          SiteReplicationEnvelope::MAX_CHUNK_SIZE, now_ms);
      break;
    case SiteReplicationState::CONFIG_METADATA:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::FLOORPLAN_CONFIG, 0, 0,
                          now_ms);
      break;
    case SiteReplicationState::IMAGE_METADATA:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::FLOORPLAN_IMAGE, 0, 0,
                          now_ms);
      break;
    case SiteReplicationState::CONFIG_PAYLOAD:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::FLOORPLAN_CONFIG,
                          this->received_bytes_,
                          std::min<uint32_t>(
                              SiteReplicationEnvelope::MAX_CHUNK_SIZE,
                              this->expected_bytes_ - this->received_bytes_),
                          now_ms);
      break;
    case SiteReplicationState::IMAGE_PAYLOAD:
      this->send_request_(identity, configuration,
                          SiteReplicationTarget::FLOORPLAN_IMAGE,
                          this->received_bytes_,
                          std::min<uint32_t>(
                              SiteReplicationEnvelope::MAX_CHUNK_SIZE,
                              this->expected_bytes_ - this->received_bytes_),
                          now_ms);
      break;
    default:
      break;
  }
}

void SiteReplicationTransport::send_request_(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot *configuration,
    SiteReplicationTarget target, uint32_t offset, uint32_t length,
    uint32_t now_ms) {
  const ParticipantCredentialRecord *credential = nullptr;
  if (target == SiteReplicationTarget::CREDENTIAL_BUNDLE) {
    credential = this->credential_session_participant_token_ != 0
                     ? this->credentials_->resolve_provisioned(
                           this->credential_session_participant_token_,
                           this->credential_session_key_version_)
                     : credential_probe_at(*this->credentials_,
                                           this->credential_probe_index_);
  } else {
    credential = select_site_sender_credential(
        identity, configuration, *this->credentials_,
        this->diagnostics_.owner_node_id);
  }
  const auto *endpoint =
      this->endpoints_->find(this->diagnostics_.owner_node_id);
  if (credential == nullptr || endpoint == nullptr || endpoint->ipv4 == 0) {
    this->diagnostics_.result = SiteReplicationResult::OWNER_OFFLINE;
    return;
  }
  SiteReplicationFrame request{};
  request.kind = SiteReplicationKind::REQUEST;
  request.target = target;
  request.key_version = credential->key_version;
  request.participant_token = credential->participant_token;
  request.request_id = ++this->request_id_;
  if (request.request_id == 0)
    request.request_id = ++this->request_id_;
  request.offset = offset;
  request.length = length;
  if (target == SiteReplicationTarget::CREDENTIAL_BUNDLE &&
      !credential_request_claim(*credential, identity.node_id,
                                &request.payload_hash,
                                &request.site_generation)) {
    this->fail_(
        SiteReplicationResult::AUTHENTICATION_FAILED,
        SiteReplicationFailureDetail::CREDENTIAL_REQUEST_CLAIM_FAILED);
    return;
  }
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  if (encode_site_replication_frame(
          request, this->packet_.data(), this->packet_.size(), &signed_size,
          &packet_size) != SiteReplicationCodecResult::OK) {
    this->fail_(
        SiteReplicationResult::AUTHENTICATION_FAILED,
        SiteReplicationFailureDetail::CREDENTIAL_REQUEST_ENCODING_FAILED);
    return;
  }
  if (!hmac_sha256(credential->key, this->packet_.data(), signed_size,
                   this->packet_.data() + signed_size)) {
    this->fail_(
        SiteReplicationResult::AUTHENTICATION_FAILED,
        SiteReplicationFailureDetail::CREDENTIAL_REQUEST_HMAC_FAILED);
    return;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(PORT);
  destination.sin_addr.s_addr = endpoint->ipv4;
  if (this->socket_->sendto(this->packet_.data(), packet_size, 0,
                            reinterpret_cast<sockaddr *>(&destination),
                            sizeof(destination)) < 0) {
    this->diagnostics_.result = SiteReplicationResult::SOCKET_ERROR;
    return;
  }
  this->expected_request_id_ = request.request_id;
  this->expected_request_key_version_ = credential->key_version;
  this->expected_request_participant_token_ =
      credential->participant_token;
  this->expected_response_target_ = target;
  this->request_sent_ms_ = now_ms;
  this->awaiting_response_ = true;
  this->diagnostics_.requests++;
}

bool SiteReplicationTransport::persist_remote_configuration_(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    SiteConfigurationSnapshot *configuration,
    SiteConfigurationStorageBackend *configuration_backend,
    SiteConfigurationStore *configuration_store, uint32_t now_ms) {
  if (configuration == nullptr || configuration_backend == nullptr ||
      configuration_store == nullptr)
    return false;
  const bool enrollment_source_valid =
      this->replication_mode_ !=
          SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT ||
      authenticated_enrollment_source_allowed(
          identity, *configuration, participants, *this->credentials_,
          this->diagnostics_.owner_node_id);
  const bool authority_rejoin_source_valid =
      this->replication_mode_ !=
          SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN ||
      authority_rejoin_payload_allowed(
          *configuration, this->workspace_->remote_configuration,
          this->endpoints_, this->authority_evidence_,
          this->diagnostics_.owner_node_id, now_ms);
  const auto validation =
      !enrollment_source_valid || !authority_rejoin_source_valid
      ? SiteConfigurationReplicationResult::OWNER_MISMATCH
      : this->workspace_->route.manifest_bound
            ? validate_replicated_floorplan_configuration(
                  identity, *configuration,
                  this->workspace_->remote_configuration,
                  this->workspace_->route)
            : validate_replicated_site_configuration(
                  identity, *configuration,
                  this->workspace_->remote_configuration,
                  this->replication_mode_,
                  this->diagnostics_.owner_node_id);
  this->diagnostics_.validation_result = validation;
  if (validation == SiteConfigurationReplicationResult::ALREADY_CURRENT)
    return true;
  if (validation != SiteConfigurationReplicationResult::OK)
    return false;
  const auto store_result =
      this->replication_mode_ ==
              SiteConfigurationReplicationMode::QUORUM_AUTHORITY_REJOIN
          ? configuration_store->replace_with_quorum_authority(
                *configuration_backend,
                this->workspace_->remote_configuration, configuration)
          : configuration_store->replace_with_newer(
                *configuration_backend,
                this->workspace_->remote_configuration, configuration);
  return store_result == SiteConfigurationStoreResult::UPDATED;
}

bool SiteReplicationTransport::persist_remote_directory_(
    SiteDirectorySnapshot *directory,
    SiteDirectoryStorageBackend *directory_backend,
    SiteDirectoryStore *directory_store) {
  if (directory == nullptr || directory_backend == nullptr ||
      directory_store == nullptr)
    return false;
  if (same_site_directory_payload(*directory,
                                  this->workspace_->remote_directory))
    return true;
  const auto result = directory_store->update(
      *directory_backend, this->workspace_->remote_directory, directory);
  return result == SiteDirectoryStoreResult::UPDATED;
}

void SiteReplicationTransport::fail_(
    SiteReplicationResult result, SiteReplicationFailureDetail detail) {
  this->diagnostics_.failure_detail = detail;
  ESP_LOGE(TAG,
           "Replication failed operation=%s state=%s result=%s detail=%s "
           "source=%s request_id=%u",
           site_replication_target_to_string(this->diagnostics_.target),
           site_replication_state_to_string(this->diagnostics_.state),
           site_replication_result_to_string(result),
           site_replication_failure_detail_to_string(detail),
           this->diagnostics_.owner_node_id[0] != '\0'
               ? this->diagnostics_.owner_node_id
               : "-",
           static_cast<unsigned>(this->expected_request_id_));
  ESP_LOGE(TAG,
           "Replication failure counters received=%u expected=%u retry=%u "
           "probes=%u requests=%u responses=%u rejected=%u",
           static_cast<unsigned>(this->received_bytes_),
           static_cast<unsigned>(this->expected_bytes_),
           static_cast<unsigned>(this->retry_count_),
           static_cast<unsigned>(this->credential_probe_index_ + 1U),
           static_cast<unsigned>(this->diagnostics_.requests),
           static_cast<unsigned>(this->diagnostics_.responses),
           static_cast<unsigned>(this->diagnostics_.rejected));
  if (this->storage_ != nullptr)
    this->storage_->cancel_floorplan_replica();
  this->replica_started_ = false;
  this->awaiting_response_ = false;
  this->expected_request_id_ = 0;
  this->expected_request_key_version_ = 0;
  this->expected_request_participant_token_ = 0;
  this->expected_response_target_ =
      SiteReplicationTarget::SITE_CONFIGURATION;
  this->diagnostics_.state =
      result == SiteReplicationResult::REVISION_CONFLICT
          ? SiteReplicationState::CONFLICT
          : SiteReplicationState::FAILED;
  this->diagnostics_.result = result;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
