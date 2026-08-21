#pragma once

#include "fixed_hmac_sha256.h"
#include "internal_ram_checkpoint.h"

#include "group_coordinator.h"
#include "participant_credentials.h"
#include "participant_discovery.h"
#include "site_coordinator.h"
#include "site_directory.h"
#include "site_group_summary.h"
#include "site_membership.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef USE_ESP32
#include "esphome/components/socket/socket.h"
#endif

namespace esphome {
namespace radar_api_server {

struct SiteHeartbeatMessage {
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t sequence{0};
  uint32_t site_term{0};
  uint32_t group_term{0};
  uint32_t lease_ms{0};
  uint32_t site_configuration_authority_epoch{0};
  uint32_t site_configuration_generation{0};
  uint64_t site_configuration_digest{0};
  uint32_t site_topology_revision{0};
  uint64_t site_topology_digest{0};
  uint32_t site_directory_revision{0};
  uint32_t site_directory_digest{0};
  bool site_credentials_complete{false};
  uint32_t floorplan_geometry_version{0};
  uint32_t floorplan_config_hash{0};
  uint32_t floorplan_image_hash{0};
  uint32_t radar_pose_version{0};
  bool floorplan_replica_ready{false};
  bool floorplan_owner_advertisement{false};
  bool group_summary_present{false};
  uint32_t group_assignment_revision{0};
  uint8_t group_participants_expected{0};
  uint8_t group_participants_online{0};
  uint32_t site_election_view_digest{0};
  uint32_t group_election_view_digest{0};
};

struct SiteHeartbeatEnvelope {
  static constexpr uint8_t CURRENT_WIRE_VERSION = 12;
  static constexpr std::size_t LEGACY_PREFIX_SIZE = 40;
  static constexpr std::size_t FLOORPLAN_PREFIX_SIZE = 60;
  static constexpr std::size_t PREVIOUS_PREFIX_SIZE = 64;
  static constexpr std::size_t GROUP_SUMMARY_PREFIX_SIZE = 72;
  static constexpr std::size_t CONFIGURATION_FENCE_PREFIX_SIZE = 88;
  static constexpr std::size_t SITE_TOPOLOGY_PREFIX_SIZE = 112;
  static constexpr std::size_t CREDENTIAL_FENCE_PREFIX_SIZE = 128;
  static constexpr std::size_t PREFIX_SIZE = 136;
  static constexpr std::size_t TAG_SIZE = 32;
  static constexpr std::size_t MAX_PACKET_SIZE = 384;

  uint32_t key_version{0};
  uint8_t wire_version{0};
  uint64_t participant_token{0};
  const uint8_t *signed_data{nullptr};
  std::size_t signed_size{0};
  const uint8_t *tag{nullptr};
  SiteHeartbeatMessage message{};
};

enum class SiteHeartbeatCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  BUFFER_TOO_SMALL,
  INVALID_PACKET,
};

SiteHeartbeatCodecResult encode_site_heartbeat(
    const SiteHeartbeatMessage &message, uint32_t key_version,
    uint64_t participant_token, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size,
    bool compatibility_v4 = false, bool compatibility_v5 = false,
    bool compatibility_v6 = false, bool compatibility_v7 = false,
    bool compatibility_v8 = false, bool compatibility_v9 = false,
    bool compatibility_v10 = false, bool compatibility_v11 = false);
SiteHeartbeatCodecResult parse_site_heartbeat(
    const uint8_t *packet, std::size_t packet_size,
    SiteHeartbeatEnvelope *envelope);
const char *site_heartbeat_codec_result_to_string(
    SiteHeartbeatCodecResult result);

struct ParticipantEndpoint {
  bool occupied{false};
  bool authenticated{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t ipv4{0};
  uint16_t port{0};
  uint32_t discovered_at_ms{0};
  uint32_t authenticated_at_ms{0};
  uint32_t site_configuration_authority_epoch{0};
  uint32_t site_configuration_generation{0};
  uint64_t site_configuration_digest{0};
  uint32_t site_topology_revision{0};
  uint64_t site_topology_digest{0};
  uint32_t site_directory_revision{0};
  uint32_t site_directory_digest{0};
  uint32_t floorplan_geometry_version{0};
  uint32_t floorplan_config_hash{0};
  uint32_t floorplan_image_hash{0};
  uint32_t radar_pose_version{0};
  bool floorplan_replica_ready{false};
  uint8_t heartbeat_wire_version{0};
  uint8_t latest_heartbeat_wire_version{0};
  bool site_credentials_complete{false};
};

bool site_heartbeat_peer_known(
    const ParticipantRegistry &participants,
    const SiteConfigurationSnapshot *configuration,
    const char *participant_id);

bool participant_endpoint_matches_configuration(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration);
bool participant_endpoint_is_bounded_roster_transition(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, uint8_t heartbeat_wire_version);
bool participant_endpoint_matches_coordination_fence(
    const ParticipantEndpoint *endpoint,
    const SiteConfigurationSnapshot &configuration,
    const char *participant_id, uint8_t heartbeat_wire_version);
bool participant_endpoint_matches_site_authority(
    const ParticipantEndpoint *endpoint, uint32_t authority_epoch,
    uint32_t generation, uint64_t configuration_digest,
    uint32_t topology_revision, uint64_t topology_digest);
bool automatic_handover_authority_available(
    const ParticipantEndpoint *configuration_owner_endpoint, uint32_t now_ms,
    uint32_t authority_epoch, uint32_t generation,
    uint64_t configuration_digest, uint32_t topology_revision,
    uint64_t topology_digest);

class ParticipantEndpointCache {
 public:
  static constexpr uint32_t AUTHENTICATED_ADDRESS_HOLD_MS = 15000;
  static constexpr uint32_t ONLINE_TIMEOUT_MS = 8000;

  bool observe_discovery(const char *participant_id, uint32_t ipv4,
                         uint16_t port, uint32_t now_ms);
  bool observe_authenticated(const char *participant_id, uint32_t ipv4,
                             uint16_t port, uint32_t now_ms);
  bool observe_floorplan_replica(const char *participant_id,
                                 uint32_t site_configuration_authority_epoch,
                                 uint32_t site_configuration_generation,
                                 uint64_t site_configuration_digest,
                                 uint32_t geometry_version,
                                 uint32_t config_hash, uint32_t image_hash,
                                 uint32_t radar_pose_version, bool ready);
  bool observe_site_topology(const char *participant_id,
                             uint32_t topology_revision,
                             uint64_t topology_digest);
  bool observe_site_directory(const char *participant_id,
                              uint32_t directory_revision,
                              uint32_t directory_digest);
  bool observe_site_credentials(const char *participant_id,
                                uint8_t heartbeat_wire_version,
                                bool credentials_complete);
  void reconcile(const ParticipantCredentialRegistry &credentials);
  void reset_authentication();
  const ParticipantEndpoint *find(const char *participant_id) const;
  const ParticipantEndpoint *endpoint_at(std::size_t slot) const;
  std::size_t size() const;
  static bool is_online(const ParticipantEndpoint *endpoint, uint32_t now_ms);
  bool needs_discovery_refresh(
      const ParticipantCredentialRegistry &credentials,
      const ParticipantRegistry &participants, uint32_t now_ms) const;
  bool needs_discovery_refresh(
      const ParticipantCredentialRegistry &credentials,
      const ParticipantRegistry &participants,
      const SiteConfigurationSnapshot *configuration,
      uint32_t now_ms) const;

 private:
  std::array<ParticipantEndpoint,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      entries_{};

  ParticipantEndpoint *ensure_(const char *participant_id);
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
};

const char *resolve_expected_group_leader(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache &endpoints,
    const char *site_leader_node_id, const char *group_id,
    uint32_t now_ms);

class SiteHeartbeatSequenceTracker {
 public:
  bool accept(uint64_t participant_token, const char *boot_id,
              uint32_t sequence);
  void reconcile(const ParticipantCredentialRegistry &credentials);

 private:
  struct Entry {
    bool occupied{false};
    uint64_t participant_token{0};
    char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
    uint32_t sequence{0};
  };

  std::array<Entry, ParticipantRegistry::MAX_REMOTE_PARTICIPANTS> entries_{};
};

enum class SiteHeartbeatResult : uint8_t {
  IDLE = 0,
  OK,
  INVALID_PACKET,
  UNKNOWN_KEY,
  AUTHENTICATION_FAILED,
  IDENTITY_MISMATCH,
  DUPLICATE_OR_REORDERED,
  SITE_MISMATCH,
  PARTICIPANT_REJECTED,
  CONFIGURATION_CONFLICT,
  SITE_REJECTED,
  GROUP_REJECTED,
  SOCKET_ERROR,
};

struct SiteHeartbeatDiagnostics {
  bool supported{true};
  bool socket_ready{false};
  uint16_t port{0};
  uint8_t endpoints{0};
  uint8_t online_endpoints{0};
  uint32_t received{0};
  uint32_t authenticated{0};
  uint32_t sent{0};
  uint32_t rejected{0};
  uint32_t address_changes{0};
  SiteHeartbeatResult last_result{SiteHeartbeatResult::IDLE};
};

struct SiteHeartbeatAuthorityProjection {
  uint32_t site_term{0};
  uint32_t election_view_digest{0};
  uint32_t compatibility_v11_election_view_digest{0};
  uint32_t previous_election_view_digest{0};
  bool control_plane_only{false};
};

SiteHeartbeatAuthorityProjection project_site_heartbeat_authority(
    const SiteCoordinatorState &state, bool control_plane_bootstrap);

const char *site_heartbeat_result_to_string(SiteHeartbeatResult result);

#ifdef USE_ESP32

class SiteHeartbeatTransport {
 public:
  static constexpr uint16_t PORT = 49191;
  static constexpr uint32_t HEARTBEAT_INTERVAL_MS =
      SiteCoordinator::HEARTBEAT_INTERVAL_MS;
  static constexpr uint32_t DISCOVERY_REFRESH_MS = 30000;
  static constexpr uint32_t SOCKET_RETRY_MS = 5000;

  SiteHeartbeatTransport();
  ~SiteHeartbeatTransport();

  void set_internal_ram_checkpoint(void *context,
                                   InternalRamCheckpointCallback callback) {
    this->ram_checkpoint_.set(context, callback);
  }

  bool setup(ParticipantCredentialRegistry *credentials,
             ParticipantDiscoveryRuntime *discovery,
             const PlatformIdentity &identity);
  void loop(const PlatformIdentity &identity,
            const SiteMembership &membership,
            const ParticipantRegistry &participants,
            const ParticipantReadinessRegistry &readiness,
            const SiteConfigurationSnapshot *configuration,
            const SiteDirectorySnapshot *directory,
            const SiteFloorplanManifest *local_floorplan,
            uint32_t local_configuration_generation,
            uint32_t local_radar_pose_version,
            bool local_floorplan_replica_ready,
            bool outbound_identity_ready,
            SiteCoordinator *site, GroupCoordinator *groups,
            SiteGroupSummaryCache *site_group_summaries,
            SiteFloorplanOwnerCache *site_floorplan_owners,
            uint32_t now_ms);

  const ParticipantEndpointCache &endpoints() const { return this->endpoints_; }
  void reset_endpoint_authentication() {
    this->endpoints_.reset_authentication();
  }
  const SiteHeartbeatDiagnostics &diagnostics() const {
    return this->diagnostics_;
  }

 private:
  static constexpr uint8_t MAX_PACKETS_PER_LOOP = 2;

  socket::Socket *socket_{nullptr};
  ParticipantCredentialRegistry *credentials_{nullptr};
  ParticipantDiscoveryRuntime *discovery_{nullptr};
  ParticipantEndpointCache endpoints_{};
  SiteHeartbeatSequenceTracker sequences_{};
  InternalRamCheckpointHook ram_checkpoint_{};
  FixedHmacSha256 hmac_{};
  std::array<uint8_t, SiteHeartbeatEnvelope::MAX_PACKET_SIZE> packet_{};
  char boot_id_[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t sequence_{0};
  uint32_t next_send_ms_{0};
  uint32_t next_discovery_ms_{0};
  uint32_t next_socket_attempt_ms_{0};
  std::size_t send_cursor_{0};
  bool send_cycle_active_{false};
  bool drain_pending_{false};
  SiteHeartbeatDiagnostics diagnostics_{};

  bool ensure_socket_(uint32_t now_ms);
  void destroy_socket_();
  void ingest_discovery_(uint32_t now_ms);
  void receive_(const PlatformIdentity &identity,
                const ParticipantRegistry &participants,
                const ParticipantReadinessRegistry &readiness,
                const SiteConfigurationSnapshot *configuration,
                const SiteDirectorySnapshot *directory,
                SiteCoordinator *site, GroupCoordinator *groups,
                SiteGroupSummaryCache *site_group_summaries,
                SiteFloorplanOwnerCache *site_floorplan_owners,
                uint32_t now_ms);
  void send_(const PlatformIdentity &identity,
             const SiteMembership &membership,
             const ParticipantRegistry &participants,
             const ParticipantReadinessRegistry &readiness,
             const SiteConfigurationSnapshot *configuration,
             const SiteDirectorySnapshot *directory,
             const SiteFloorplanManifest *local_floorplan,
             uint32_t local_configuration_generation,
             uint32_t local_radar_pose_version,
             bool local_floorplan_replica_ready,
             bool outbound_identity_ready,
             const SiteCoordinator &site, const GroupCoordinator &groups,
             uint32_t now_ms);
  bool send_message_(const SiteHeartbeatMessage &message,
                     const ParticipantCredentialRecord &credential,
                     const ParticipantEndpoint &endpoint,
                     bool compatibility_v4 = false,
                     bool compatibility_v5 = false,
                     bool compatibility_v6 = false,
                      bool compatibility_v7 = false,
                      bool compatibility_v8 = false,
                      bool compatibility_v9 = false,
                       bool compatibility_v10 = false,
                       bool compatibility_v11 = false);
  SiteHeartbeatResult process_packet_(
      const uint8_t *packet, std::size_t packet_size, uint32_t source_ipv4,
      uint16_t source_port, const PlatformIdentity &identity,
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const SiteConfigurationSnapshot *configuration,
      const SiteDirectorySnapshot *directory, SiteCoordinator *site,
      GroupCoordinator *groups, SiteGroupSummaryCache *site_group_summaries,
      SiteFloorplanOwnerCache *site_floorplan_owners,
      uint32_t now_ms);
  void update_diagnostics_(uint32_t now_ms);
};

static_assert(sizeof(SiteHeartbeatTransport) <= 6144,
              "heartbeat transport exceeded the embedded memory budget");

#endif

static_assert(std::is_trivially_copyable<SiteHeartbeatMessage>::value,
              "site heartbeat messages must remain fixed-size values");
static_assert(
    std::is_trivially_copyable<SiteHeartbeatAuthorityProjection>::value,
    "heartbeat authority projections must remain fixed-size values");
static_assert(std::is_trivially_copyable<ParticipantEndpointCache>::value,
              "endpoint cache must not own heap-backed state");
static_assert(sizeof(ParticipantEndpointCache) <= 3328,
              "endpoint cache exceeded the embedded memory budget");
static_assert(std::is_trivially_copyable<SiteHeartbeatSequenceTracker>::value,
              "heartbeat replay state must not own heap-backed state");
static_assert(sizeof(SiteHeartbeatSequenceTracker) <= 2048,
              "heartbeat replay state exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
