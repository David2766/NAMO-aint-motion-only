#pragma once

#include "credential_provisioning.h"
#include "participant_credentials.h"
#include "radar_storage.h"
#include "site_configuration.h"
#include "site_directory.h"
#include "site_heartbeat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef USE_ESP32
#include "esphome/components/socket/socket.h"
#endif

namespace esphome {
namespace radar_api_server {

enum class SiteReplicationKind : uint8_t {
  REQUEST = 1,
  RESPONSE = 2,
};

enum class SiteReplicationTarget : uint8_t {
  SITE_CONFIGURATION = 0,
  FLOORPLAN_CONFIG = 1,
  FLOORPLAN_IMAGE = 2,
  SITE_DIRECTORY = 3,
  CREDENTIAL_BUNDLE = 4,
};

enum class SiteReplicationWireStatus : uint8_t {
  OK = 0,
  NOT_OWNER,
  NOT_COMMITTED,
  NOT_FOUND,
  INVALID_RANGE,
  STORAGE_FAILED,
  PARTICIPANT_REJECTED,
};

struct SiteReplicationFrame {
  SiteReplicationKind kind{SiteReplicationKind::REQUEST};
  SiteReplicationTarget target{SiteReplicationTarget::SITE_CONFIGURATION};
  uint32_t key_version{0};
  uint64_t participant_token{0};
  uint32_t request_id{0};
  uint32_t offset{0};
  uint32_t length{0};
  uint32_t total_size{0};
  uint32_t payload_hash{0};
  uint32_t site_generation{0};
  SiteReplicationWireStatus status{SiteReplicationWireStatus::OK};
  const uint8_t *body{nullptr};
};

struct SiteReplicationEnvelope {
  static constexpr std::size_t PREFIX_SIZE = 48;
  static constexpr std::size_t TAG_SIZE = 32;
  static constexpr std::size_t MAX_CHUNK_SIZE = 384;
  static constexpr std::size_t MAX_PACKET_SIZE =
      PREFIX_SIZE + MAX_CHUNK_SIZE + TAG_SIZE;

  SiteReplicationFrame frame{};
  const uint8_t *signed_data{nullptr};
  std::size_t signed_size{0};
  const uint8_t *tag{nullptr};
};

enum class SiteReplicationCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  BUFFER_TOO_SMALL,
  INVALID_PACKET,
};

SiteReplicationCodecResult encode_site_replication_frame(
    const SiteReplicationFrame &frame, uint8_t *packet,
    std::size_t packet_capacity, std::size_t *signed_size,
    std::size_t *packet_size);
SiteReplicationCodecResult parse_site_replication_frame(
    const uint8_t *packet, std::size_t packet_size,
    SiteReplicationEnvelope *envelope);

enum class SiteReplicationState : uint8_t {
  IDLE = 0,
  WAITING_FOR_OWNER,
  SITE_CONFIGURATION,
  DIRECTORY_METADATA,
  DIRECTORY_PAYLOAD,
  DIRECTORY_COMMITTING,
  CREDENTIAL_METADATA,
  CREDENTIAL_PAYLOAD,
  CREDENTIAL_COMMITTING,
  CONFIG_METADATA,
  IMAGE_METADATA,
  CONFIG_PAYLOAD,
  IMAGE_PAYLOAD,
  COMMITTING,
  COMPLETE,
  CONFLICT,
  FAILED,
};

enum class SiteReplicationResult : uint8_t {
  IDLE = 0,
  OK,
  BUSY,
  INVALID_OWNER,
  OWNER_OFFLINE,
  CREDENTIAL_MISSING,
  SOCKET_ERROR,
  TIMEOUT,
  INVALID_PACKET,
  AUTHENTICATION_FAILED,
  REMOTE_REJECTED,
  INVALID_CONFIGURATION,
  REVISION_CONFLICT,
  PAYLOAD_MISMATCH,
  STORAGE_FAILED,
};

enum class SiteReplicationFailureDetail : uint8_t {
  NONE = 0,
  CREDENTIAL_PROBE_REJECTED,
  CREDENTIAL_PROBE_TIMEOUT,
  CREDENTIAL_PAYLOAD_CRYPTO_FAILED,
  CREDENTIAL_BUNDLE_BINDING_MISMATCH,
  CREDENTIAL_REQUEST_CLAIM_FAILED,
  CREDENTIAL_REQUEST_ENCODING_FAILED,
  CREDENTIAL_REQUEST_HMAC_FAILED,
};

const char *site_replication_state_to_string(SiteReplicationState state);
const char *site_replication_result_to_string(SiteReplicationResult result);
const char *site_replication_failure_detail_to_string(
    SiteReplicationFailureDetail detail);
const char *site_replication_target_to_string(SiteReplicationTarget target);
SiteReplicationResult site_replication_response_result(
    SiteReplicationWireStatus status, uint32_t payload_hash);
bool site_replication_response_matches_request(
    const SiteReplicationFrame &frame, uint32_t expected_request_id,
    SiteReplicationTarget expected_target);

static constexpr uint32_t SITE_REPLICATION_SLOT_MS = 2500;
static constexpr uint8_t SITE_REPLICATION_MAX_SESSION_RETRIES = 5;
static constexpr uint32_t SITE_REPLICATION_BUSY_PAYLOAD_HASH = 0x42555359;
static constexpr uint32_t SITE_REPLICATION_BUSY_RETRY_DELAY_MS =
    SITE_REPLICATION_SLOT_MS + 250;
static constexpr uint8_t SITE_REPLICATION_MAX_BUSY_RETRIES = 6;

uint32_t site_replication_slot_delay_ms(
    const SiteConfigurationSnapshot &configuration,
    const char *local_node_id);
uint32_t site_replication_source_slot_delay_ms(
    const SiteConfigurationSnapshot &configuration,
    const char *source_node_id, const char *local_node_id);
uint32_t site_replication_retry_delay_ms(uint8_t retry_count);
bool site_replication_busy_retry_allowed(uint8_t retry_count);

enum class SiteReplicationRouteResult : uint8_t {
  OK = 0,
  LEGACY_FALLBACK,
  INVALID_ARGUMENT,
  INVALID_DIRECTORY,
  ASSIGNMENT_MISSING,
  OWNER_MISMATCH,
  LOCAL_OWNER,
};

struct SiteReplicationRoute {
  bool manifest_bound{false};
  FloorplanDirectoryEntry floorplan{};
};

struct SiteAuthorityReplicationEvidence {
  uint32_t authority_epoch{0};
  uint32_t generation{0};
  uint64_t configuration_digest{0};
  uint32_t topology_revision{0};
  uint64_t topology_digest{0};
  uint8_t endorsements{0};
  uint8_t quorum_required{0};

  bool confirmed() const {
    return this->authority_epoch != 0 && this->generation != 0 &&
           this->configuration_digest != 0 &&
           this->topology_revision != 0 && this->topology_digest != 0 &&
           this->quorum_required != 0 &&
           this->endorsements >= this->quorum_required;
  }
};

static_assert(
    std::is_trivially_copyable<SiteAuthorityReplicationEvidence>::value,
    "site authority evidence must remain trivially copyable");
static_assert(sizeof(SiteAuthorityReplicationEvidence) <= 40,
              "site authority evidence exceeded its embedded budget");

uint8_t site_authority_rejoin_quorum_required(
    const SiteConfigurationSnapshot &configuration);
bool site_authority_rejoin_evidence_matches(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache &endpoints,
    const SiteAuthorityReplicationEvidence &evidence, uint32_t now_ms);
bool site_configuration_replica_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *requester_participant_id);

enum class PairingSiteSyncDecision : uint8_t {
  NOT_APPLICABLE = 0,
  START_STRICT,
  ALREADY_CURRENT,
  OWNER_OFFLINE,
};

PairingSiteSyncDecision pairing_site_sync_decision(
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRecord *participant,
    const ParticipantCredentialRecord *credential,
    const ParticipantEndpoint *endpoint, const char *local_node_id,
    const char *requested_owner_node_id, bool local_floorplan_replica_ready,
    uint32_t now_ms);

SiteReplicationRouteResult resolve_site_replication_route(
    const SiteDirectorySnapshot *directory,
    const SiteConfigurationSnapshot &configuration,
    const char *local_node_id, const char *requested_owner_node_id,
    SiteReplicationRoute *route);
bool site_replication_route_matches_configuration(
    const SiteReplicationRoute &route,
    const SiteConfigurationSnapshot &configuration);
SiteConfigurationReplicationResult
validate_replicated_floorplan_configuration(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &current,
    const SiteConfigurationSnapshot &remote,
    const SiteReplicationRoute &route);

enum class SiteDirectoryReplicationValidation : uint8_t {
  OK = 0,
  ALREADY_CURRENT,
  INVALID_DIRECTORY,
  STALE_REMOTE,
  REVISION_CONFLICT,
};

enum class SiteDirectoryReplicationMode : uint8_t {
  STANDARD = 0,
  INITIAL_BOOTSTRAP,
};

SiteDirectoryReplicationValidation validate_replicated_site_directory(
    const char *site_id, const SiteDirectorySnapshot &current,
    const SiteDirectorySnapshot &remote,
    bool allow_authority_repair = false);
bool site_replication_participant_authorized(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id);
bool site_directory_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site);
bool site_directory_replication_required(
    const SiteCoordinatorState &site,
    const SiteConfigurationSnapshot &configuration,
    const char *local_participant_id);
const char *site_directory_bootstrap_source(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials, uint32_t now_ms);
bool site_directory_bootstrap_request_authorized(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints, uint32_t now_ms);
bool site_directory_mirror_promotion_allowed(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory,
    const SiteCoordinatorState &site,
    const ParticipantEndpointCache &endpoints,
    const ParticipantCredentialRegistry &credentials, uint32_t now_ms);

struct SiteReplicationDiagnostics {
  bool supported{true};
  bool socket_ready{false};
  SiteReplicationState state{SiteReplicationState::IDLE};
  SiteReplicationResult result{SiteReplicationResult::IDLE};
  SiteReplicationFailureDetail failure_detail{
      SiteReplicationFailureDetail::NONE};
  SiteConfigurationReplicationResult validation_result{
      SiteConfigurationReplicationResult::NOT_EVALUATED};
  SiteConfigurationReplicationMode mode{
      SiteConfigurationReplicationMode::STRICT};
  SiteReplicationTarget target{SiteReplicationTarget::SITE_CONFIGURATION};
  char owner_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t remote_generation{0};
  uint32_t transferred_bytes{0};
  uint32_t total_bytes{0};
  uint32_t requests{0};
  uint32_t responses{0};
  uint32_t retries{0};
  uint32_t rejected{0};
  uint32_t replayed{0};
  uint32_t stale_requests{0};
};

bool site_replication_session_matches(
    const SiteReplicationDiagnostics &diagnostics, const char *owner_node_id,
    SiteConfigurationReplicationMode mode);

enum class SiteReplicationSourceAdmission : uint8_t {
  ACQUIRED = 0,
  CONTINUED,
  BUSY,
  INVALID_ARGUMENT,
};

struct SiteReplicationSourceDiagnostics {
  bool active{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t accepted{0};
  uint32_t busy{0};
  uint32_t completed{0};
  uint32_t expired{0};
};

class SiteReplicationSourceLease {
 public:
  static constexpr uint32_t LEASE_MS = SITE_REPLICATION_SLOT_MS;

  SiteReplicationSourceAdmission admit(const char *participant_id,
                                       uint32_t source_ipv4,
                                       uint32_t now_ms);
  bool release(const char *participant_id, uint32_t source_ipv4,
               bool completed);
  bool expire(uint32_t now_ms);
  uint32_t remaining_ms(uint32_t now_ms) const;

  const SiteReplicationSourceDiagnostics &diagnostics() const {
    return this->diagnostics_;
  }

 private:
  SiteReplicationSourceDiagnostics diagnostics_{};
  uint32_t source_ipv4_{0};
  uint32_t expires_ms_{0};

  void clear_active_();
};

enum class SiteReplicationReplayAdmission : uint8_t {
  NEW_REQUEST = 0,
  DUPLICATE,
  STALE,
};

class SiteReplicationReplayCache {
 public:
  SiteReplicationReplayAdmission classify(
      uint64_t participant_token, uint32_t source_ipv4,
      const SiteReplicationFrame &request, uint32_t now_ms);
  void remember(uint64_t participant_token, uint32_t source_ipv4,
                const SiteReplicationFrame &request, const uint8_t *packet,
                std::size_t packet_size, uint32_t now_ms);
  const uint8_t *packet() const { return this->packet_.data(); }
  uint16_t packet_size() const { return this->packet_size_; }

 private:
  bool valid_{false};
  uint64_t participant_token_{0};
  uint32_t source_ipv4_{0};
  uint32_t request_id_{0};
  uint32_t offset_{0};
  uint32_t length_{0};
  uint32_t expires_at_ms_{0};
  SiteReplicationTarget target_{SiteReplicationTarget::SITE_CONFIGURATION};
  uint16_t packet_size_{0};
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet_{};

  void clear_();
};

struct SiteReplicationWorkspace {
  SiteConfigurationStorageBlob received_configuration{};
  SiteConfigurationStorageBlob served_configuration{};
  SiteConfigurationSnapshot remote_configuration{};
  SiteDirectorySnapshot remote_directory{};
  ParticipantCredentialSnapshot remote_credentials{};
  ParticipantCredentialRegistry credential_merge_scratch{};
  SiteReplicationRoute route{};
  SiteReplicationReplayCache replay{};
};

static_assert(std::is_trivially_copyable<SiteReplicationWorkspace>::value,
              "site replication workspace must not own heap-backed state");
static_assert(sizeof(SiteReplicationWorkspace) <= 20 * 1024,
              "site replication workspace exceeded its PSRAM budget");

#ifdef USE_ESP32

class SiteReplicationTransport {
 public:
  static constexpr uint16_t PORT = 49192;

  SiteReplicationTransport();
  ~SiteReplicationTransport();

  bool setup(RadarStorage *storage,
             ParticipantCredentialSnapshot *credential_snapshot,
             ParticipantCredentialStorageBackend *credential_backend,
             ParticipantCredentialStore *credential_store,
             const ParticipantEndpointCache *endpoints);
  SiteReplicationResult start(const char *owner_node_id, uint32_t now_ms,
                              SiteConfigurationReplicationMode mode =
                                  SiteConfigurationReplicationMode::STRICT,
                              const SiteReplicationRoute *route = nullptr,
                              const SiteAuthorityReplicationEvidence *evidence =
                                  nullptr);
  SiteReplicationResult start_directory(const char *source_node_id,
                                        uint32_t now_ms,
                                        SiteDirectoryReplicationMode mode =
                                            SiteDirectoryReplicationMode::STANDARD);
  SiteReplicationResult start_credentials(
      const char *owner_node_id, uint32_t now_ms,
      bool authority_bootstrap = false);
  void cancel();
  void loop(const PlatformIdentity &identity,
            const ParticipantRegistry &participants,
            SiteConfigurationSnapshot *configuration,
            SiteConfigurationStorageBackend *configuration_backend,
            SiteConfigurationStore *configuration_store,
            SiteDirectorySnapshot *directory,
            SiteDirectoryStorageBackend *directory_backend,
            SiteDirectoryStore *directory_store,
            const SiteCoordinatorState &site, uint32_t now_ms);

  const SiteReplicationDiagnostics &diagnostics() const {
    return this->diagnostics_;
  }
  const SiteReplicationSourceDiagnostics &source_diagnostics() const {
    return this->source_lease_.diagnostics();
  }
  uint32_t source_lease_remaining_ms(uint32_t now_ms) const {
    return this->source_lease_.remaining_ms(now_ms);
  }

 private:
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 600;
  static constexpr uint32_t OWNER_DISCOVERY_TIMEOUT_MS = 15000;
  static constexpr uint8_t MAX_RETRIES = 6;
  static constexpr uint32_t SOCKET_RETRY_MS = 5000;
  static constexpr uint8_t MAX_PACKETS_PER_LOOP = 2;

  socket::Socket *socket_{nullptr};
  RadarStorage *storage_{nullptr};
  ParticipantCredentialSnapshot *credential_snapshot_{nullptr};
  ParticipantCredentialRegistry *credentials_{nullptr};
  ParticipantCredentialStorageBackend *credential_backend_{nullptr};
  ParticipantCredentialStore *credential_store_{nullptr};
  const ParticipantEndpointCache *endpoints_{nullptr};
  SiteReplicationWorkspace *workspace_{nullptr};
  std::array<uint8_t, SiteReplicationEnvelope::MAX_PACKET_SIZE> packet_{};
  SiteReplicationDiagnostics diagnostics_{};
  SiteReplicationSourceLease source_lease_{};
  uint32_t request_id_{0};
  uint32_t expected_request_id_{0};
  uint32_t expected_request_key_version_{0};
  uint64_t expected_request_participant_token_{0};
  uint32_t received_bytes_{0};
  uint32_t expected_bytes_{0};
  uint32_t expected_hash_{0};
  uint32_t config_size_{0};
  uint32_t image_size_{0};
  uint32_t config_hash_{0};
  uint32_t image_hash_{0};
  uint32_t session_id_{0};
  uint32_t session_started_ms_{0};
  uint32_t request_sent_ms_{0};
  uint32_t busy_retry_due_ms_{0};
  uint32_t next_socket_attempt_ms_{0};
  uint8_t retry_count_{0};
  uint8_t busy_retry_count_{0};
  uint8_t credential_probe_index_{0};
  uint32_t credential_session_key_version_{0};
  uint64_t credential_session_participant_token_{0};
  bool credential_authority_bootstrap_{false};
  SiteDirectoryReplicationMode directory_mode_{
      SiteDirectoryReplicationMode::STANDARD};
  SiteAuthorityReplicationEvidence authority_evidence_{};
  SiteReplicationTarget expected_response_target_{
      SiteReplicationTarget::SITE_CONFIGURATION};
  bool awaiting_response_{false};
  bool drain_pending_{false};
  bool replica_started_{false};
  SiteConfigurationReplicationMode replication_mode_{
      SiteConfigurationReplicationMode::STRICT};

  bool allocate_workspace_();
  void destroy_workspace_();
  bool ensure_socket_(uint32_t now_ms);
  void destroy_socket_();
  void receive_(const PlatformIdentity &identity,
                const ParticipantRegistry &participants,
                const SiteConfigurationSnapshot &configuration,
                const SiteDirectorySnapshot *directory,
                const SiteCoordinatorState &site,
                uint32_t now_ms);
  void serve_request_(const SiteReplicationEnvelope &envelope,
                      const ParticipantCredentialRecord &credential,
                      uint32_t source_ipv4, uint16_t source_port,
                      const PlatformIdentity &identity,
                      const ParticipantRegistry &participants,
                      const SiteConfigurationSnapshot &configuration,
                      const SiteDirectorySnapshot *directory,
                      const SiteCoordinatorState &site, uint32_t now_ms);
  void accept_response_(const SiteReplicationEnvelope &envelope,
                        const ParticipantCredentialRecord &credential,
                        uint32_t source_ipv4,
                        const SiteConfigurationSnapshot &configuration,
                        const SiteDirectorySnapshot *directory,
                        const SiteCoordinatorState &site,
                        uint32_t now_ms);
  void advance_(const PlatformIdentity &identity,
                const ParticipantRegistry &participants,
                SiteConfigurationSnapshot *configuration,
                SiteConfigurationStorageBackend *configuration_backend,
                SiteConfigurationStore *configuration_store,
                SiteDirectorySnapshot *directory,
                SiteDirectoryStorageBackend *directory_backend,
                SiteDirectoryStore *directory_store,
                const SiteCoordinatorState &site,
                uint32_t now_ms);
  void send_request_(const PlatformIdentity &identity,
                     const SiteConfigurationSnapshot *configuration,
                     SiteReplicationTarget target, uint32_t offset,
                     uint32_t length, uint32_t now_ms);
  bool persist_remote_configuration_(
      const PlatformIdentity &identity,
      const ParticipantRegistry &participants,
      SiteConfigurationSnapshot *configuration,
      SiteConfigurationStorageBackend *configuration_backend,
      SiteConfigurationStore *configuration_store, uint32_t now_ms);
  bool persist_remote_directory_(
      SiteDirectorySnapshot *directory,
      SiteDirectoryStorageBackend *directory_backend,
      SiteDirectoryStore *directory_store);
  void fail_(SiteReplicationResult result,
             SiteReplicationFailureDetail detail =
                 SiteReplicationFailureDetail::NONE);
};

static_assert(sizeof(SiteReplicationTransport) <= 2048,
              "site replication transport exceeded its embedded budget");

#endif

static_assert(std::is_trivially_copyable<SiteReplicationRoute>::value,
              "site replication route must not own heap-backed state");
static_assert(sizeof(SiteReplicationRoute) <= 192,
              "site replication route exceeded its embedded budget");
static_assert(std::is_trivially_copyable<SiteReplicationSourceLease>::value,
              "site replication source lease must remain fixed-size");
static_assert(sizeof(SiteReplicationSourceLease) <= 128,
              "site replication source lease exceeded its embedded budget");
static_assert(std::is_trivially_copyable<SiteReplicationReplayCache>::value,
              "site replication replay cache must remain fixed-size");
static_assert(sizeof(SiteReplicationReplayCache) <= 512,
              "site replication replay cache exceeded its embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
