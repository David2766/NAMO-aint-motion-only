#pragma once

#include "participant_registry.h"
#include "platform_identity.h"
#include "role_quorum.h"
#include "role_election_fence.h"
#include "site_configuration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class SiteHeartbeatAdmissionResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  UNKNOWN_PARTICIPANT,
  PARTICIPANT_INACTIVE,
  STALE_TERM,
  TERM_SOURCE_INELIGIBLE,
  EXPIRED,
  DUPLICATE_OR_REORDERED,
  FULL,
};

struct SiteLeaderHeartbeat {
  static constexpr std::size_t MAX_BOOT_ID_LENGTH = 48;

  const char *participant_id{nullptr};
  const char *boot_id{nullptr};
  uint32_t sequence{0};
  uint32_t term{0};
  uint32_t lease_ms{0};
  uint32_t sample_age_ms{0};
  uint32_t directory_revision{0};
  uint32_t directory_digest{0};
  uint32_t roster_revision{0};
  uint32_t election_view_digest{0};
  uint8_t election_contract_version{3};
  bool credentials_complete{true};
};

struct SiteCoordinatorState {
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t term{0};
  uint32_t lease_expires_at_ms{0};
  uint8_t participants_expected{1};
  uint8_t participants_online{1};
  uint8_t quorum_required{1};
  bool local_leader{true};
  bool quorum_available{true};
  bool configuration_ready{false};
  uint32_t roster_revision{0};
  uint32_t election_view_digest{0};
  uint32_t compatibility_v11_election_view_digest{0};
  uint32_t previous_election_view_digest{0};
  uint8_t election_endorsements{0};
  bool election_fenced{false};
};

class SiteCoordinator {
 public:
  static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;
  static constexpr uint32_t DEFAULT_LEASE_MS = 6000;
  static constexpr uint32_t MIN_LEASE_MS = 2000;
  static constexpr uint32_t MAX_LEASE_MS = 15000;

  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants, uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const SiteConfigurationSnapshot *configuration,
              uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const SiteConfigurationSnapshot *configuration,
              bool local_credentials_complete, uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const SiteConfigurationSnapshot *configuration,
              const SiteDirectorySnapshot *directory,
              bool local_credentials_complete, uint32_t now_ms);
  SiteHeartbeatAdmissionResult accept_heartbeat(
      const ParticipantRegistry &participants,
      const SiteLeaderHeartbeat &heartbeat, uint32_t now_ms);
  SiteHeartbeatAdmissionResult accept_heartbeat(
      const ParticipantRegistry &participants,
      const SiteConfigurationSnapshot *configuration,
      const SiteLeaderHeartbeat &heartbeat, uint32_t now_ms);
  void set_local_directory_fence(uint32_t revision, uint32_t digest);
  void clear();

  const SiteCoordinatorState &state() const { return this->state_; }
  uint32_t generation() const { return this->generation_; }
  static uint32_t lease_remaining_ms(const SiteCoordinatorState &state,
                                     uint32_t now_ms);

 private:
  struct HeartbeatSlot {
    bool occupied{false};
    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char boot_id[SiteLeaderHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
    uint32_t sequence{0};
    uint32_t term{0};
    uint32_t expires_at_ms{0};
    uint32_t directory_revision{0};
    uint32_t directory_digest{0};
    uint32_t roster_revision{0};
    uint32_t election_view_digest{0};
    uint8_t election_contract_version{3};
    bool credentials_complete{true};
    bool term_admitted{false};
  };

  SiteCoordinatorState state_{};
  std::array<HeartbeatSlot, ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      heartbeats_{};
  uint32_t generation_{0};
  char suspended_leader_node_id_[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t suspended_leader_term_{0};
  uint32_t local_directory_revision_{0};
  uint32_t local_directory_digest_{0};
  uint32_t initial_probe_started_ms_{0};
  uint32_t standalone_authority_term_{0};
  bool initial_probe_active_{false};
  bool initial_probe_complete_{false};
  bool local_credentials_complete_{true};
  bool standalone_authority_held_{false};
  bool standalone_handoff_pending_{false};
  uint32_t election_roster_revision_{0};
  RoleElectionTransitionViews transition_views_{};

  HeartbeatSlot *find_heartbeat_(const char *participant_id);
  HeartbeatSlot *ensure_heartbeat_(const char *participant_id);
  void reconcile_heartbeat_slots_(const ParticipantRegistry &participants,
                                  const SiteConfigurationSnapshot *configuration,
                                  uint32_t now_ms);
  void elect_leader_(const PlatformIdentity &identity,
                     const ParticipantRegistry &participants,
                     const SiteConfigurationSnapshot *configuration,
                     const SiteDirectorySnapshot *directory,
                     uint32_t now_ms);

  static bool valid_text_(const char *value, std::size_t max_length);
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
  static void copy_(char *target, std::size_t capacity, const char *source);
};

const char *site_heartbeat_admission_result_to_string(
    SiteHeartbeatAdmissionResult result);

static_assert(std::is_trivially_copyable<SiteCoordinator>::value,
              "site coordinator must not own heap-backed state");
static_assert(sizeof(SiteCoordinator) <= 4096,
              "site coordinator memory exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
