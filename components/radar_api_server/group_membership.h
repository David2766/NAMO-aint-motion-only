#pragma once

#include "group_roster.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct GroupMembershipMember {
  bool occupied{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  bool leader_eligible{false};
};

struct GroupMembershipView {
  static constexpr std::size_t MAX_PARTICIPANTS = 7;
  static constexpr std::size_t MAX_REMOTE_PARTICIPANTS =
      MAX_PARTICIPANTS - 1;

  bool valid{false};
  bool local_admitted{false};
  bool over_capacity{false};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t assignment_revision{0};
  uint8_t member_count{0};
  uint8_t configured_member_count{0};
  std::array<GroupMembershipMember, MAX_PARTICIPANTS> members{};

  const GroupMembershipMember *find(const char *participant_id) const;
  bool contains(const char *participant_id) const {
    return this->find(participant_id) != nullptr;
  }
};

enum class GroupMembershipResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  CONFIGURATION_UNAVAILABLE,
  LOCAL_ASSIGNMENT_UNAVAILABLE,
  LOCAL_NOT_ADMITTED,
};

GroupMembershipResult build_local_group_membership(
    const SiteConfigurationSnapshot *configuration,
    const char *local_participant_id, GroupMembershipView *view);
GroupMembershipResult build_local_group_membership(
    const GroupRosterSnapshot *roster, const char *local_participant_id,
    GroupMembershipView *view);
bool build_canonical_group_runtime_records(
    const SiteConfigurationSnapshot *configuration,
    const char *local_participant_id, const char *participant_id,
    ParticipantRecord *participant,
    ParticipantReadinessRecord *readiness);
bool build_group_roster_runtime_records(
    const GroupRosterSnapshot *roster, const char *local_participant_id,
    const char *participant_id, ParticipantRecord *participant,
    ParticipantReadinessRecord *readiness);
const char *group_membership_result_to_string(GroupMembershipResult result);

static_assert(std::is_trivially_copyable<GroupMembershipView>::value,
              "Group membership view must not own heap-backed state");
static_assert(sizeof(GroupMembershipView) <= 768,
              "Group membership view exceeded its embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
