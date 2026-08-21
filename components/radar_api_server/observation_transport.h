#pragma once

#include "fixed_hmac_sha256.h"
#include "internal_ram_checkpoint.h"

#include "fusion_snapshot.h"
#include "local_observation.h"
#include "participant_credentials.h"
#include "participant_readiness.h"
#include "site_space_summary.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef USE_ESP32
#include "esphome/components/socket/socket.h"
#endif

namespace esphome {
namespace radar_api_server {

struct PlatformIdentity;
struct GroupCoordinatorState;
struct ParticipantEndpoint;
struct SpatialProductionFusionOutput;
class GroupCoordinator;
class ParticipantEndpointCache;

enum class AuthenticatedTransportMessageType : uint8_t {
  OBSERVATION = 1,
  FUSION_SNAPSHOT = 2,
  SITE_SPACE_SUMMARY = 3,
};

struct AuthenticatedObservationEnvelope {
  static constexpr std::size_t PREFIX_SIZE = 24;
  static constexpr std::size_t TAG_SIZE = 32;
  static constexpr std::size_t MAX_PACKET_SIZE = 768;
  static constexpr std::size_t MAX_PAYLOAD_SIZE =
      MAX_PACKET_SIZE - PREFIX_SIZE - TAG_SIZE;

  AuthenticatedTransportMessageType message_type{
      AuthenticatedTransportMessageType::OBSERVATION};
  uint32_t key_version{0};
  uint64_t participant_token{0};
  const uint8_t *payload{nullptr};
  uint16_t payload_size{0};
  const uint8_t *tag{nullptr};
};

bool parse_authenticated_observation_envelope(
    const uint8_t *packet, std::size_t packet_size,
    AuthenticatedObservationEnvelope *envelope);

enum class ObservationEncodeResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  OBSERVATION_EXPIRED,
  BUFFER_TOO_SMALL,
};

const char *observation_encode_result_to_string(
    ObservationEncodeResult result);

struct ObservationEncodeContext {
  const char *site_id{nullptr};
  const char *participant_id{nullptr};
  const char *group_id{nullptr};
  const char *boot_id{nullptr};
  uint32_t sequence{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  uint16_t capability_flags{0};
  uint8_t max_targets{0};
  uint32_t now_ms{0};
  const LocalObservation *observation{nullptr};
};

ObservationEncodeResult encode_observation_messagepack(
    const ObservationEncodeContext &context, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size);
bool encode_authenticated_observation_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size);
bool encode_authenticated_fusion_snapshot_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size);
bool encode_authenticated_site_space_summary_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size);

struct FusionSnapshotEncodeContext {
  const char *site_id{nullptr};
  const char *leader_node_id{nullptr};
  const char *group_id{nullptr};
  const char *boot_id{nullptr};
  uint32_t sequence{0};
  uint32_t group_term{0};
  uint32_t ttl_ms{FusionSnapshot::DEFAULT_TTL_MS};
  const SpatialShadowFusionOutput *fusion{nullptr};
};

ObservationEncodeResult encode_fusion_snapshot_messagepack(
    const FusionSnapshotEncodeContext &context, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size);
ObservationEncodeResult encode_site_space_summary_messagepack(
    const SiteSpaceSummary &summary, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size);

struct ObservationTransportRateState {
  bool occupied{false};
  uint64_t participant_token{0};
  uint32_t rate_window_started_ms{0};
  uint8_t accepted_in_window{0};
};

class ObservationTransportRateLimiter {
 public:
  void clear();
  bool allow(uint64_t participant_token, uint32_t now_ms);

 private:
  static constexpr uint32_t RATE_WINDOW_MS = 1000;
  static constexpr uint8_t MAX_PACKETS_PER_WINDOW = 3;

  std::array<ObservationTransportRateState,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      entries_{};
};

enum class ObservationTransportResult : uint8_t {
  IDLE = 0,
  OK,
  PACKET_TOO_LARGE,
  INVALID_ENVELOPE,
  UNKNOWN_KEY,
  AUTHENTICATION_FAILED,
  RATE_LIMITED,
  INVALID_MESSAGEPACK,
  INVALID_OBSERVATION,
  IDENTITY_MISMATCH,
  ADMISSION_REJECTED,
  SOCKET_ERROR,
};

const char *observation_transport_result_to_string(
    ObservationTransportResult result);

enum class ObservationSendResult : uint8_t {
  IDLE = 0,
  OK,
  GROUP_UNAVAILABLE,
  LOCAL_LEADER,
  PARTICIPANT_INACTIVE,
  READINESS_UNAVAILABLE,
  POSE_MISMATCH,
  CREDENTIAL_UNAVAILABLE,
  ENDPOINT_OFFLINE,
  OBSERVATION_UNAVAILABLE,
  ENCODE_FAILED,
  SOCKET_ERROR,
};

const char *observation_send_result_to_string(ObservationSendResult result);
ObservationSendResult evaluate_observation_send(
    const char *local_participant_id,
    const GroupCoordinatorState *selected_group, bool multiple_groups,
    const ParticipantRecord *leader,
    const ParticipantReadinessRecord *leader_readiness,
    const ParticipantCredentialRecord *leader_credential,
    const ParticipantEndpoint *leader_endpoint, uint32_t local_pose_version,
    uint32_t now_ms);

struct ObservationTransportDiagnostics {
  bool supported{true};
  bool smoke_mode{false};
  bool socket_ready{false};
  uint16_t port{0};
  uint32_t received{0};
  uint32_t authenticated{0};
  uint32_t admitted{0};
  uint32_t rejected{0};
  uint32_t rate_limited{0};
  uint32_t max_process_us{0};
  uint8_t pending{0};
  bool sender_enabled{false};
  uint32_t sent{0};
  uint32_t send_skipped{0};
  uint32_t max_encode_us{0};
  uint32_t max_send_us{0};
  uint32_t fusion_snapshot_received{0};
  uint32_t fusion_snapshot_admitted{0};
  uint32_t fusion_snapshot_sent{0};
  uint32_t fusion_snapshot_rejected{0};
  uint32_t fusion_snapshot_expired{0};
  uint32_t site_space_summary_received{0};
  uint32_t site_space_summary_admitted{0};
  uint32_t site_space_summary_sent{0};
  uint32_t site_space_summary_rejected{0};
  char send_target[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char fusion_snapshot_source[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  ObservationTransportResult last_result{ObservationTransportResult::IDLE};
  ObservationAdmissionResult last_admission{ObservationAdmissionResult::OK};
  FusionSnapshotAdmissionResult last_fusion_snapshot_admission{
      FusionSnapshotAdmissionResult::OK};
  SiteSpaceSummaryAdmissionResult last_site_space_summary_admission{
      SiteSpaceSummaryAdmissionResult::OK};
  ObservationSendResult last_send_result{ObservationSendResult::IDLE};
};

struct ObservationTransportTrack {
  bool valid{false};
  bool velocity_available{false};
  bool uncertainty_available{false};
  uint32_t local_track_id{0};
  uint32_t sample_age_ms{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
  float variance_x_mm2{0.0f};
  float variance_y_mm2{0.0f};
  uint8_t quality_score{0};
};

struct ObservationTransportSample {
  static constexpr std::size_t MAX_TRACKS = 3;

  bool occupied{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint64_t participant_token{0};
  uint32_t sequence{0};
  uint32_t received_at_ms{0};
  uint32_t expires_at_ms{0};
  uint32_t sample_age_ms{0};
  uint32_t ttl_ms{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  uint8_t track_count{0};
  bool presence{false};
  bool motion{false};
  std::array<ObservationTransportTrack, MAX_TRACKS> tracks{};
};

class ObservationTransportInbox {
 public:
  bool store(const ObservationTransportSample &sample);
  bool copy_latest(const char *participant_id, uint32_t now_ms,
                   ObservationTransportSample *sample) const;
  void expire(uint32_t now_ms);
  void clear();
  uint8_t size() const;

 private:
  std::array<ObservationTransportSample,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      samples_{};
};

#ifdef USE_ESP32

class ObservationTransportParser;

class ObservationTransport {
 public:
  const char *boot_id() const { return this->boot_id_; }
  static constexpr uint16_t PORT = 49190;
  static constexpr uint32_t SOCKET_RETRY_MS = 5000;
  static constexpr const char *SMOKE_PARTICIPANT_ID =
      "satellite-smoke-transport";

  ObservationTransport();
  ~ObservationTransport();

  void set_internal_ram_checkpoint(void *context,
                                   InternalRamCheckpointCallback callback) {
    this->ram_checkpoint_.set(context, callback);
  }

  bool setup(ParticipantCredentialRegistry *credentials);
  bool setup_smoke(ParticipantCredentialRegistry *credentials);
  void loop(const ParticipantRegistry &participants,
            const ParticipantReadinessRegistry &readiness,
            const GroupCoordinator &groups,
            const SiteConfigurationSnapshot *configuration,
            const SiteCoordinatorState &site,
            SiteSpaceSummaryCache *site_space_summaries,
            const char *local_participant_id, const char *site_id,
            uint32_t now_ms);
  void loop(const ParticipantRegistry &participants,
            const ParticipantReadinessRegistry &readiness,
            const GroupCoordinator &groups,
            const SiteConfigurationSnapshot *configuration,
            const GroupRosterSnapshot *group_roster,
            const SiteCoordinatorState &site,
            SiteSpaceSummaryCache *site_space_summaries,
            const char *local_participant_id, const char *site_id,
            uint32_t now_ms);
  void send(const PlatformIdentity &identity,
            const ParticipantRegistry &participants,
            const ParticipantReadinessRegistry &readiness,
            const ParticipantEndpointCache &endpoints,
            const GroupCoordinator &groups,
            const SiteConfigurationSnapshot *configuration,
            const LocalObservation &observation,
            uint32_t local_pose_version, const char *site_id,
            uint32_t now_ms);
  void send(const PlatformIdentity &identity,
            const ParticipantRegistry &participants,
            const ParticipantReadinessRegistry &readiness,
            const ParticipantEndpointCache &endpoints,
            const GroupCoordinator &groups,
            const SiteConfigurationSnapshot *configuration,
            const GroupRosterSnapshot *group_roster,
            const LocalObservation &observation,
            uint32_t local_pose_version, const char *site_id,
            uint32_t now_ms);
  void send_fusion_snapshot(
      const PlatformIdentity &identity,
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const ParticipantEndpointCache &endpoints,
      const GroupCoordinator &groups,
      const SiteConfigurationSnapshot *configuration,
      const SpatialShadowFusionOutput &fusion,
      const SpatialProductionFusionOutput &production,
      const char *site_id, uint32_t now_ms);
  void send_fusion_snapshot(
      const PlatformIdentity &identity,
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const ParticipantEndpointCache &endpoints,
      const GroupCoordinator &groups,
      const SiteConfigurationSnapshot *configuration,
      const GroupRosterSnapshot *group_roster,
      const SpatialShadowFusionOutput &fusion,
      const SpatialProductionFusionOutput &production,
      const char *site_id, uint32_t now_ms);
  void send_site_space_summary(
      const PlatformIdentity &identity,
      const ParticipantEndpointCache &endpoints,
      const GroupCoordinator &groups, const SiteCoordinatorState &site,
      const SiteConfigurationSnapshot *configuration,
      const SpatialProductionFusionOutput &production,
      const char *site_id, SiteSpaceSummaryCache *site_space_summaries,
      uint32_t now_ms);

  const ObservationTransportDiagnostics &diagnostics() const {
    return this->diagnostics_;
  }
  bool copy_latest(const char *participant_id, uint32_t now_ms,
                   ObservationTransportSample *sample) const {
    return this->inbox_ != nullptr &&
           this->inbox_->copy_latest(participant_id, now_ms, sample);
  }
  bool copy_fusion_snapshot(uint32_t now_ms,
                            FusionSnapshot *snapshot) const {
    return this->fusion_snapshot_cache_ != nullptr &&
           this->fusion_snapshot_cache_->copy(now_ms, snapshot);
  }

 private:
  static constexpr std::size_t RECEIVE_BUFFER_SIZE =
      AuthenticatedObservationEnvelope::MAX_PACKET_SIZE + 1;
  static constexpr uint8_t MAX_PACKETS_PER_LOOP = 2;

  socket::Socket *socket_{nullptr};
  ObservationTransportParser *parser_{nullptr};
  ObservationTransportInbox *inbox_{nullptr};
  ParticipantCredentialRegistry *credentials_{nullptr};
  ParticipantCredentialRecord smoke_credential_{};
  ParticipantRecord smoke_participant_{};
  ParticipantReadinessRecord smoke_readiness_{};
  ObservationTransportRateLimiter rate_limiter_{};
  ObservationAdmission admission_{};
  InternalRamCheckpointHook ram_checkpoint_{};
  FixedHmacSha256 hmac_{};
  std::array<uint8_t, RECEIVE_BUFFER_SIZE> receive_buffer_{};
  std::array<uint8_t, AuthenticatedObservationEnvelope::MAX_PACKET_SIZE>
      send_buffer_{};
  char boot_id_[49]{};
  uint32_t send_sequence_{0};
  uint32_t next_send_ms_{0};
  uint32_t fusion_snapshot_sequence_{0};
  uint32_t next_fusion_snapshot_send_ms_{0};
  uint32_t site_space_summary_sequence_{0};
  uint32_t next_site_space_summary_send_ms_{0};
  uint8_t fusion_snapshot_send_cursor_{0};
  std::array<uint32_t, ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      fusion_snapshot_sent_at_ms_{};
  FusionSnapshotCache *fusion_snapshot_cache_{nullptr};
  ObservationTransportDiagnostics diagnostics_{};
  bool inbox_in_psram_{false};
  bool fusion_snapshot_cache_in_psram_{false};
  bool parser_in_psram_{false};
  bool drain_pending_{false};
  uint32_t next_socket_attempt_ms_{0};

  bool setup_runtime_(ParticipantCredentialRegistry *credentials);
  bool allocate_parser_();
  void destroy_parser_();
  bool ensure_socket_(uint32_t now_ms);
  void destroy_socket_();
  ObservationTransportResult process_packet_(
      const uint8_t *packet, std::size_t packet_size,
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const GroupCoordinator &groups,
      const SiteConfigurationSnapshot *configuration,
      const GroupRosterSnapshot *group_roster,
      const SiteCoordinatorState &site,
      SiteSpaceSummaryCache *site_space_summaries,
      const char *local_participant_id, const char *site_id, uint32_t now_ms);
  void queue_(const ObservationTransportSample &sample);
  void expire_(uint32_t now_ms);
  void update_pending_count_();
  bool allocate_inbox_();
  void destroy_inbox_();
  bool allocate_fusion_snapshot_cache_();
  void destroy_fusion_snapshot_cache_();
  void skip_send_(ObservationSendResult result, const char *target = nullptr);
};

#endif

static_assert(std::is_trivially_copyable<ObservationTransportRateState>::value,
              "transport rate state must not own heap-backed state");
static_assert(sizeof(ObservationTransportRateLimiter) <= 512,
              "transport rate limiter exceeded the embedded budget");
static_assert(ObservationTransportSample::MAX_TRACKS == 3,
              "transport observations must remain bounded to three tracks");
static_assert(std::is_trivially_copyable<ObservationTransportSample>::value,
              "transport samples must not own heap-backed state");
static_assert(sizeof(ObservationTransportInbox) <= 6144,
              "transport inbox exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
