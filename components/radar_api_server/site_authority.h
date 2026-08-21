#pragma once

#include "platform_identity.h"
#include "site_configuration.h"
#include "site_heartbeat.h"
#include "site_replication.h"

#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class SiteAuthorityState : uint8_t {
  NOT_READY = 0,
  STANDALONE,
  CONSISTENT,
  CONVERGING,
  CONFLICT,
};

enum class SiteAuthorityReason : uint8_t {
  NOT_READY = 0,
  NONE,
  UNFENCED_PEER,
  LOCAL_BEHIND,
  PEER_BEHIND,
  DIVERGENT_DIGEST,
};

struct SiteAuthoritySnapshot {
  uint64_t local_digest{0};
  uint32_t local_authority_epoch{0};
  uint32_t local_generation{0};
  uint32_t evaluated_at_ms{0};
  SiteAuthorityState state{SiteAuthorityState::NOT_READY};
  SiteAuthorityReason reason{SiteAuthorityReason::NOT_READY};
  bool evaluated{false};
  bool spatial_consistent{false};

  bool consistent() const {
    return this->state == SiteAuthorityState::STANDALONE ||
           this->state == SiteAuthorityState::CONSISTENT;
  }

  bool spatially_consistent() const {
    return this->spatial_consistent;
  }

  bool describes_revision(
      const SiteConfigurationSnapshot &configuration) const {
    return this->evaluated &&
           this->local_authority_epoch == configuration.authority_epoch &&
           this->local_generation == configuration.generation;
  }
};

enum class SiteActivationAuthorityGateResult : uint8_t {
  OK = 0,
  CONTINUITY_UNAVAILABLE,
  CONFLICT,
};

SiteAuthoritySnapshot evaluate_site_authority(
    const PlatformIdentity *identity,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms);
SiteActivationAuthorityGateResult evaluate_site_activation_authority_gate(
    const SiteAuthoritySnapshot &authority,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpoint *endpoint, bool endpoint_online);

struct SiteAuthorityReplicationCandidate {
  bool available{false};
  char source_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t authority_epoch{0};
  uint32_t generation{0};
  uint64_t configuration_digest{0};
  uint32_t slot_delay_ms{0};
  SiteConfigurationReplicationMode mode{
      SiteConfigurationReplicationMode::STRICT};
  SiteReplicationRoute route{};
  SiteAuthorityReplicationEvidence evidence{};
};

SiteAuthorityReplicationCandidate select_enrollment_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantRegistry &participants,
    const ParticipantCredentialRegistry &credentials,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms);
bool automatic_enrollment_floorplan_replication_allowed(
    const SiteAuthorityReplicationCandidate &candidate,
    const ParticipantEndpoint *source_endpoint,
    bool local_floorplan_stored, uint32_t local_config_hash,
    uint32_t local_image_hash);

SiteAuthorityReplicationCandidate select_site_authority_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms);
SiteAuthorityReplicationCandidate select_floorplan_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms);
SiteAuthorityReplicationCandidate select_site_replication_candidate(
    const PlatformIdentity &identity,
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const ParticipantEndpointCache *endpoints, uint32_t now_ms);

class SiteAuthorityReplicationTracker {
 public:
  static constexpr uint32_t FAILURE_COOLDOWN_MS = 60000;
  void reset();
  void observe_candidate(const SiteAuthorityReplicationCandidate &candidate,
                         uint32_t now_ms);
  void observe_transport(const SiteConfigurationSnapshot &local,
                         SiteReplicationState transport_state,
                         uint32_t now_ms);
  bool start_due(uint32_t now_ms,
                 SiteReplicationState transport_state) const;
  void complete_start(SiteReplicationResult result, uint32_t now_ms);

  uint32_t pending_authority_epoch() const {
    return this->pending_.authority_epoch;
  }
  uint32_t pending_generation() const { return this->pending_.generation; }
  uint32_t active_generation() const { return this->active_.generation; }
  uint32_t blocked_generation() const { return this->blocked_.generation; }
  uint8_t retry_count() const { return this->retry_count_; }
  const char *source_node_id() const { return this->source_node_id_; }
  bool allows_owner_handover() const {
    return this->replication_mode_ ==
           SiteConfigurationReplicationMode::AUTOMATIC_OWNER_HANDOVER;
  }
  SiteConfigurationReplicationMode replication_mode() const {
    return this->replication_mode_;
  }
  const SiteReplicationRoute *route() const {
    return this->route_.manifest_bound ? &this->route_ : nullptr;
  }
  bool busy() const {
    return this->pending_.generation != 0 || this->active_.generation != 0;
  }
  uint32_t wait_remaining_ms(uint32_t now_ms) const;

 private:
  struct Revision {
    uint32_t authority_epoch{0};
    uint32_t generation{0};
    uint64_t candidate_fingerprint{0};
  };

  static uint64_t candidate_fingerprint_(
      const SiteAuthorityReplicationCandidate &candidate);
  static bool same_candidate_(const Revision &revision,
                              uint32_t authority_epoch,
                              uint32_t generation,
                              uint64_t candidate_fingerprint);
  static bool transport_available_(SiteReplicationState state);
  void schedule_retry_(const Revision &revision, uint32_t now_ms);

  Revision pending_{};
  Revision active_{};
  Revision blocked_{};
  uint32_t due_ms_{0};
  uint8_t retry_count_{0};
  char source_node_id_[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  SiteConfigurationReplicationMode replication_mode_{
      SiteConfigurationReplicationMode::STRICT};
  SiteReplicationRoute route_{};
};

static_assert(std::is_trivially_copyable<SiteAuthoritySnapshot>::value,
              "Site authority snapshot must remain trivially copyable");
static_assert(sizeof(SiteAuthoritySnapshot) <= 32,
              "Site authority snapshot exceeded its embedded budget");
static_assert(std::is_trivially_copyable<SiteAuthorityReplicationTracker>::value,
              "Site authority replication tracker must remain trivially copyable");
static_assert(sizeof(SiteAuthorityReplicationTracker) <= 288,
              "Site authority replication tracker exceeded its embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
