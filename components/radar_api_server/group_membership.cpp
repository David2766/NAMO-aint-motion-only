#include "group_membership.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

void copy_text(char *target, std::size_t capacity, const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  if (source == nullptr) {
    target[0] = '\0';
    return;
  }
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

bool valid_local_assignment(const SiteConfigurationMember *member) {
  return member != nullptr &&
         member->type == ParticipantType::NAMO_NODE &&
         member->membership == ParticipantMembership::ACTIVE &&
         member->spatial_profile.committed &&
         member->spatial_profile.group_id[0] != '\0' &&
         member->spatial_profile.space_id[0] != '\0';
}

constexpr uint16_t NAMO_RUNTIME_CAPABILITIES =
    PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
    PARTICIPANT_CAP_POSITION_UNCERTAINTY | PARTICIPANT_CAP_RANGE |
    PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;

}  // namespace

const GroupMembershipMember *GroupMembershipView::find(
    const char *participant_id) const {
  if (!this->valid || participant_id == nullptr ||
      participant_id[0] == '\0')
    return nullptr;
  for (const auto &member : this->members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, participant_id) == 0)
      return &member;
  }
  return nullptr;
}

GroupMembershipResult build_local_group_membership(
    const SiteConfigurationSnapshot *configuration,
    const char *local_participant_id, GroupMembershipView *view) {
  if (view == nullptr || local_participant_id == nullptr ||
      local_participant_id[0] == '\0')
    return GroupMembershipResult::INVALID_ARGUMENT;
  *view = GroupMembershipView{};
  if (configuration == nullptr || !configuration->roster_committed)
    return GroupMembershipResult::CONFIGURATION_UNAVAILABLE;

  const auto *local = configuration->find(local_participant_id);
  if (!valid_local_assignment(local))
    return GroupMembershipResult::LOCAL_ASSIGNMENT_UNAVAILABLE;

  view->valid = true;
  copy_text(view->group_id, sizeof(view->group_id),
            local->spatial_profile.group_id);
  copy_text(view->space_id, sizeof(view->space_id),
            local->spatial_profile.space_id);
  if (configuration->floorplan.committed) {
    copy_text(view->floorplan_id, sizeof(view->floorplan_id),
              configuration->floorplan.floorplan_id);
  }
  view->assignment_revision = configuration->roster_revision;

  for (const auto &member : configuration->members) {
    if (valid_local_assignment(&member) &&
        std::strcmp(member.spatial_profile.group_id, view->group_id) == 0 &&
        view->configured_member_count != UINT8_MAX)
      ++view->configured_member_count;
  }

  for (const auto &member : configuration->members) {
    if (!valid_local_assignment(&member) ||
        std::strcmp(member.spatial_profile.group_id, view->group_id) != 0)
      continue;
    if (view->member_count >= view->members.size()) {
      view->over_capacity = true;
      continue;
    }

    auto &selected = view->members[view->member_count++];
    selected.occupied = true;
    copy_text(selected.participant_id, sizeof(selected.participant_id),
              member.participant_id);
    selected.leader_eligible =
        member.type == ParticipantType::NAMO_NODE &&
        member.leader_eligible;
  }

  view->local_admitted = view->contains(local_participant_id);
  return view->local_admitted ? GroupMembershipResult::OK
                              : GroupMembershipResult::LOCAL_NOT_ADMITTED;
}

GroupMembershipResult build_local_group_membership(
    const GroupRosterSnapshot *roster, const char *local_participant_id,
    GroupMembershipView *view) {
  if (view == nullptr || local_participant_id == nullptr ||
      local_participant_id[0] == '\0')
    return GroupMembershipResult::INVALID_ARGUMENT;
  *view = GroupMembershipView{};
  if (roster == nullptr || !valid_group_roster(*roster))
    return GroupMembershipResult::CONFIGURATION_UNAVAILABLE;

  view->valid = true;
  copy_text(view->group_id, sizeof(view->group_id), roster->group_id);
  copy_text(view->space_id, sizeof(view->space_id), roster->space_id);
  copy_text(view->floorplan_id, sizeof(view->floorplan_id),
            roster->floorplan.floorplan_id);
  view->assignment_revision = roster->assignment_revision;
  view->configured_member_count =
      static_cast<uint8_t>(roster->member_count());
  const auto *local = roster->find(local_participant_id);
  if (local == nullptr)
    return GroupMembershipResult::LOCAL_ASSIGNMENT_UNAVAILABLE;
  auto &local_selected = view->members[view->member_count++];
  local_selected.occupied = true;
  copy_text(local_selected.participant_id,
            sizeof(local_selected.participant_id), local->participant_id);
  local_selected.leader_eligible = local->leader_eligible;
  for (const auto &member : roster->members) {
    if (!member.occupied ||
        std::strcmp(member.participant_id, local_participant_id) == 0)
      continue;
    auto &selected = view->members[view->member_count++];
    selected.occupied = true;
    copy_text(selected.participant_id, sizeof(selected.participant_id),
              member.participant_id);
    selected.leader_eligible = member.leader_eligible;
  }
  view->local_admitted = view->contains(local_participant_id);
  return view->local_admitted ? GroupMembershipResult::OK
                              : GroupMembershipResult::LOCAL_NOT_ADMITTED;
}

bool build_canonical_group_runtime_records(
    const SiteConfigurationSnapshot *configuration,
    const char *local_participant_id, const char *participant_id,
    ParticipantRecord *participant,
    ParticipantReadinessRecord *readiness) {
  if (participant == nullptr || readiness == nullptr)
    return false;
  *participant = ParticipantRecord{};
  *readiness = ParticipantReadinessRecord{};
  if (configuration == nullptr || !configuration->roster_committed ||
      !configuration->floorplan.committed || local_participant_id == nullptr ||
      participant_id == nullptr ||
      std::strlen(configuration->floorplan.floorplan_id) >
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH)
    return false;

  const auto *local = configuration->find(local_participant_id);
  const auto *member = configuration->find(participant_id);
  if (!valid_local_assignment(local) || !valid_local_assignment(member) ||
      std::strcmp(local->spatial_profile.group_id,
                  member->spatial_profile.group_id) != 0 ||
      local->spatial_profile.pose_version == 0 ||
      local->spatial_profile.calibration_version == 0 ||
      member->spatial_profile.pose_version == 0 ||
      member->spatial_profile.calibration_version == 0 ||
      !valid_spatial_transform(member->spatial_profile.transform))
    return false;

  participant->occupied = true;
  copy_text(participant->participant_id, sizeof(participant->participant_id),
            member->participant_id);
  participant->type = member->type;
  participant->trust = member->trust;
  participant->membership = member->membership;
  participant->leader_eligible = member->leader_eligible;

  readiness->occupied = true;
  copy_text(readiness->participant_id, sizeof(readiness->participant_id),
            member->participant_id);
  auto &runtime = readiness->config;
  runtime.capability_flags = NAMO_RUNTIME_CAPABILITIES;
  runtime.max_targets = 3;
  copy_text(runtime.floorplan_id, sizeof(runtime.floorplan_id),
            configuration->floorplan.floorplan_id);
  copy_text(runtime.space_id, sizeof(runtime.space_id),
            member->spatial_profile.space_id);
  copy_text(runtime.group_id, sizeof(runtime.group_id),
            member->spatial_profile.group_id);
  runtime.geometry_version = configuration->floorplan.geometry_version;
  runtime.local_pose_version = local->spatial_profile.pose_version;
  runtime.pose_version = member->spatial_profile.pose_version;
  runtime.calibration_version = member->spatial_profile.calibration_version;
  runtime.transform = member->spatial_profile.transform;
  return ParticipantReadinessRegistry::valid_config(runtime);
}

bool build_group_roster_runtime_records(
    const GroupRosterSnapshot *roster, const char *local_participant_id,
    const char *participant_id, ParticipantRecord *participant,
    ParticipantReadinessRecord *readiness) {
  if (participant == nullptr || readiness == nullptr)
    return false;
  *participant = ParticipantRecord{};
  *readiness = ParticipantReadinessRecord{};
  if (roster == nullptr || !valid_group_roster(*roster) ||
      local_participant_id == nullptr || participant_id == nullptr ||
      std::strlen(roster->floorplan.floorplan_id) >
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH)
    return false;
  const auto *local = roster->find(local_participant_id);
  const auto *member = roster->find(participant_id);
  if (local == nullptr || member == nullptr ||
      local->spatial_profile.pose_version == 0 ||
      local->spatial_profile.calibration_version == 0 ||
      member->spatial_profile.pose_version == 0 ||
      member->spatial_profile.calibration_version == 0 ||
      !valid_spatial_transform(member->spatial_profile.transform))
    return false;

  participant->occupied = true;
  copy_text(participant->participant_id, sizeof(participant->participant_id),
            member->participant_id);
  participant->type = member->type;
  participant->trust = member->trust;
  participant->membership = member->membership;
  participant->leader_eligible = member->leader_eligible;

  readiness->occupied = true;
  copy_text(readiness->participant_id, sizeof(readiness->participant_id),
            member->participant_id);
  auto &runtime = readiness->config;
  runtime.capability_flags = NAMO_RUNTIME_CAPABILITIES;
  runtime.max_targets = 3;
  copy_text(runtime.floorplan_id, sizeof(runtime.floorplan_id),
            roster->floorplan.floorplan_id);
  copy_text(runtime.space_id, sizeof(runtime.space_id), roster->space_id);
  copy_text(runtime.group_id, sizeof(runtime.group_id), roster->group_id);
  runtime.geometry_version = roster->floorplan.geometry_version;
  runtime.local_pose_version = local->spatial_profile.pose_version;
  runtime.pose_version = member->spatial_profile.pose_version;
  runtime.calibration_version = member->spatial_profile.calibration_version;
  runtime.transform = member->spatial_profile.transform;
  return ParticipantReadinessRegistry::valid_config(runtime);
}

const char *group_membership_result_to_string(GroupMembershipResult result) {
  switch (result) {
    case GroupMembershipResult::OK:
      return "ok";
    case GroupMembershipResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case GroupMembershipResult::CONFIGURATION_UNAVAILABLE:
      return "configuration_unavailable";
    case GroupMembershipResult::LOCAL_ASSIGNMENT_UNAVAILABLE:
      return "local_assignment_unavailable";
    case GroupMembershipResult::LOCAL_NOT_ADMITTED:
      return "local_not_admitted";
  }
  return "invalid_argument";
}

}  // namespace radar_api_server
}  // namespace esphome
