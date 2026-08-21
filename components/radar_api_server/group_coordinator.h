#pragma once

#include "group_membership.h"
#include "participant_readiness.h"
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

class ParticipantCredentialRegistry;
struct SiteDirectorySnapshot;

enum class GroupHeartbeatResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  UNKNOWN_PARTICIPANT,
  PARTICIPANT_INACTIVE,
  GROUP_MISMATCH,
  STALE_TERM,
  TERM_SOURCE_INELIGIBLE,
  EXPIRED,
  DUPLICATE_OR_REORDERED,
  FULL,
};

struct GroupHeartbeat {
  static constexpr std::size_t MAX_BOOT_ID_LENGTH = 48;

  const char *participant_id{nullptr};
  const char *group_id{nullptr};
  const char *boot_id{nullptr};
  uint32_t sequence{0};
  uint32_t term{0};
  uint32_t lease_ms{0};
  uint32_t sample_age_ms{0};
  uint32_t roster_revision{0};
  uint32_t assignment_revision{0};
  uint32_t election_view_digest{0};
  uint8_t election_contract_version{3};
  bool credentials_complete{true};
};

struct GroupCoordinatorState {
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t term{0};
  uint32_t assignment_revision{0};
  uint32_t lease_expires_at_ms{0};
  uint8_t participants_expected{0};
  uint8_t participants_online{0};
  uint8_t quorum_required{0};
  bool configured{false};
  bool local_leader{true};
  bool quorum_available{false};
  bool canonical_assignment{false};
  bool credentials_complete{false};
  uint32_t election_view_digest{0};
  uint32_t compatibility_v11_election_view_digest{0};
  uint32_t previous_election_view_digest{0};
  uint32_t legacy_election_view_digest{0};
  uint8_t election_endorsements{0};
  bool election_fenced{false};
};

class GroupCoordinator {
 public:
  static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;
  static constexpr uint32_t DEFAULT_LEASE_MS = 6000;
  static constexpr uint32_t MIN_LEASE_MS = 2000;
  static constexpr uint32_t MAX_LEASE_MS = 15000;
  static constexpr std::size_t MAX_GROUPS = 1;

  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const ParticipantReadinessRegistry &readiness, uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const ParticipantReadinessRegistry &readiness,
              const SiteConfigurationSnapshot *configuration,
              uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const ParticipantReadinessRegistry &readiness,
              const SiteConfigurationSnapshot *configuration,
              const char *site_leader_node_id, uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const ParticipantReadinessRegistry &readiness,
              const SiteConfigurationSnapshot *configuration,
              const ParticipantCredentialRegistry *credentials,
              const SiteDirectorySnapshot *directory,
              const char *site_leader_node_id, uint32_t now_ms);
  void update(const PlatformIdentity &identity,
              const ParticipantRegistry &participants,
              const ParticipantReadinessRegistry &readiness,
              const SiteConfigurationSnapshot *configuration,
              const GroupRosterSnapshot *group_roster,
              const ParticipantCredentialRegistry *credentials,
              const SiteDirectorySnapshot *directory,
              const char *site_leader_node_id, uint32_t now_ms);
  GroupHeartbeatResult accept_heartbeat(
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const GroupHeartbeat &heartbeat, uint32_t now_ms);
  GroupHeartbeatResult accept_heartbeat(
      const ParticipantRegistry &participants,
      const ParticipantReadinessRegistry &readiness,
      const SiteConfigurationSnapshot *configuration,
      const GroupHeartbeat &heartbeat, uint32_t now_ms);
  void clear();

  std::size_t group_count() const;
  const GroupCoordinatorState *group_at(std::size_t slot) const;
  const GroupCoordinatorState *local_group(
      const SiteConfigurationSnapshot *configuration,
      const char *local_participant_id) const;
  const GroupMembershipView *local_membership() const {
    return this->local_membership_.valid ? &this->local_membership_ : nullptr;
  }
  uint32_t generation() const { return this->generation_; }
  static uint32_t lease_remaining_ms(const GroupCoordinatorState &state,
                                     uint32_t now_ms);

 private:
  struct GroupSlot {
    bool occupied{false};
    bool seen{false};
    char suspended_leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
    uint32_t suspended_leader_term{0};
    uint8_t previous_expected{0};
    uint8_t previous_online{0};
    uint32_t election_roster_revision{0};
    uint32_t election_assignment_revision{0};
    RoleElectionTransitionViews transition_views{};
    GroupCoordinatorState state{};
  };

  struct HeartbeatSlot {
    bool occupied{false};
    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
    uint32_t sequence{0};
    uint32_t term{0};
    uint32_t expires_at_ms{0};
    bool term_admitted{false};
    uint32_t roster_revision{0};
    uint32_t assignment_revision{0};
    uint32_t election_view_digest{0};
    uint8_t election_contract_version{3};
    bool credentials_complete{true};
  };

  std::array<GroupSlot, MAX_GROUPS> groups_{};
  std::array<HeartbeatSlot, GroupMembershipView::MAX_REMOTE_PARTICIPANTS>
      heartbeats_{};
  GroupMembershipView local_membership_{};
  uint32_t generation_{0};

  GroupSlot *find_group_(const char *group_id);
  GroupSlot *ensure_group_(const char *group_id, bool configured);
  HeartbeatSlot *find_heartbeat_(const char *participant_id);
  HeartbeatSlot *ensure_heartbeat_(const char *participant_id);
  void reconcile_heartbeat_slots_(const ParticipantRegistry &participants,
                                  const ParticipantReadinessRegistry &readiness,
                                  const SiteConfigurationSnapshot *configuration,
                                  uint32_t now_ms);
  void elect_leader_(GroupSlot *group, const PlatformIdentity &identity,
                     const ParticipantRegistry &participants,
                     const ParticipantReadinessRegistry &readiness,
                     const SiteConfigurationSnapshot *configuration,
                     const GroupRosterSnapshot *group_roster,
                     const ParticipantCredentialRegistry *credentials,
                     const SiteDirectorySnapshot *directory,
                     const char *site_leader_node_id,
                     bool local_candidate_allowed, uint32_t now_ms);

  static bool valid_text_(const char *value, std::size_t max_length);
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
  static void copy_(char *target, std::size_t capacity, const char *source);
};

const char *group_heartbeat_result_to_string(GroupHeartbeatResult result);

static_assert(std::is_trivially_copyable<GroupCoordinator>::value,
              "group coordinator must not own heap-backed state");
static_assert(sizeof(GroupCoordinator) <= 6144,
              "group coordinator memory exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
