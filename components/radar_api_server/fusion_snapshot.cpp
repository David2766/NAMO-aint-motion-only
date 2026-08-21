#include "fusion_snapshot.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool valid_text(const char *value) {
  return value != nullptr && value[0] != '\0';
}

bool valid_target(const FusionSnapshotTarget &target) {
  return target.valid && target.fused_track_id != 0 &&
         (target.state == SpatialShadowTrackState::CONFIRMED ||
          target.state == SpatialShadowTrackState::COASTING) &&
         target.source_count != 0 && target.confidence <= 100 &&
         std::isfinite(target.x_mm) && std::isfinite(target.y_mm) &&
         std::isfinite(target.vx_mm_s) && std::isfinite(target.vy_mm_s) &&
         target.x_mm >= -100000.0f && target.x_mm <= 100000.0f &&
         target.y_mm >= -100000.0f && target.y_mm <= 100000.0f &&
         target.vx_mm_s >= -20000.0f && target.vx_mm_s <= 20000.0f &&
         target.vy_mm_s >= -20000.0f && target.vy_mm_s <= 20000.0f;
}

bool valid_snapshot(const FusionSnapshot &snapshot) {
  if (!snapshot.occupied || !valid_text(snapshot.site_id) ||
      !valid_text(snapshot.leader_node_id) ||
      !valid_text(snapshot.group_id) || !valid_text(snapshot.boot_id) ||
      snapshot.sequence == 0 || snapshot.group_term == 0 ||
      snapshot.source_sequence == 0 ||
      snapshot.ttl_ms < FusionSnapshot::MIN_TTL_MS ||
      snapshot.ttl_ms > FusionSnapshot::MAX_TTL_MS ||
      snapshot.target_count > snapshot.targets.size())
    return false;
  uint8_t valid_count = 0;
  for (const auto &target : snapshot.targets) {
    if (!target.valid)
      continue;
    if (!valid_target(target))
      return false;
    ++valid_count;
  }
  return valid_count == snapshot.target_count;
}

}  // namespace

const char *fusion_snapshot_admission_result_to_string(
    FusionSnapshotAdmissionResult result) {
  switch (result) {
    case FusionSnapshotAdmissionResult::OK: return "ok";
    case FusionSnapshotAdmissionResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case FusionSnapshotAdmissionResult::SITE_MISMATCH:
      return "site_mismatch";
    case FusionSnapshotAdmissionResult::UNKNOWN_PARTICIPANT:
      return "unknown_participant";
    case FusionSnapshotAdmissionResult::PARTICIPANT_INACTIVE:
      return "participant_inactive";
    case FusionSnapshotAdmissionResult::READINESS_UNAVAILABLE:
      return "readiness_unavailable";
    case FusionSnapshotAdmissionResult::GROUP_MISMATCH:
      return "group_mismatch";
    case FusionSnapshotAdmissionResult::LEADER_MISMATCH:
      return "leader_mismatch";
    case FusionSnapshotAdmissionResult::TERM_MISMATCH:
      return "term_mismatch";
  }
  return "invalid_argument";
}

FusionSnapshotAdmissionResult evaluate_fusion_snapshot_admission(
    const FusionSnapshot &snapshot, const char *authenticated_participant_id,
    const char *local_site_id, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupCoordinatorState *group) {
  return evaluate_fusion_snapshot_admission(
      snapshot, authenticated_participant_id, local_site_id,
      participants.find(authenticated_participant_id),
      readiness.find(authenticated_participant_id), group);
}

FusionSnapshotAdmissionResult evaluate_fusion_snapshot_admission(
    const FusionSnapshot &snapshot, const char *authenticated_participant_id,
    const char *local_site_id, const ParticipantRecord *participant,
    const ParticipantReadinessRecord *readiness,
    const GroupCoordinatorState *group) {
  if (!valid_snapshot(snapshot) ||
      !valid_text(authenticated_participant_id) ||
      !valid_text(local_site_id) || group == nullptr || !group->configured)
    return FusionSnapshotAdmissionResult::INVALID_ARGUMENT;
  if (std::strcmp(snapshot.site_id, local_site_id) != 0)
    return FusionSnapshotAdmissionResult::SITE_MISMATCH;
  if (std::strcmp(snapshot.leader_node_id,
                  authenticated_participant_id) != 0)
    return FusionSnapshotAdmissionResult::LEADER_MISMATCH;
  if (participant == nullptr)
    return FusionSnapshotAdmissionResult::UNKNOWN_PARTICIPANT;
  if (participant->membership != ParticipantMembership::ACTIVE)
    return FusionSnapshotAdmissionResult::PARTICIPANT_INACTIVE;
  if (!ParticipantReadinessRegistry::ready(readiness) ||
      (readiness->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) == 0)
    return FusionSnapshotAdmissionResult::READINESS_UNAVAILABLE;
  if (std::strcmp(snapshot.group_id, group->group_id) != 0)
    return FusionSnapshotAdmissionResult::GROUP_MISMATCH;
  if (group->leader_node_id[0] == '\0' ||
      std::strcmp(snapshot.leader_node_id, group->leader_node_id) != 0 ||
      group->local_leader)
    return FusionSnapshotAdmissionResult::LEADER_MISMATCH;
  if (snapshot.group_term != group->term)
    return FusionSnapshotAdmissionResult::TERM_MISMATCH;
  return FusionSnapshotAdmissionResult::OK;
}

bool FusionSnapshotCache::store(const FusionSnapshot &snapshot) {
  if (!valid_snapshot(snapshot))
    return false;
  if (this->snapshot_.occupied &&
      std::strcmp(this->snapshot_.leader_node_id,
                  snapshot.leader_node_id) == 0 &&
      std::strcmp(this->snapshot_.boot_id, snapshot.boot_id) == 0 &&
      snapshot.sequence <= this->snapshot_.sequence)
    return false;
  this->snapshot_ = snapshot;
  return true;
}

bool FusionSnapshotCache::copy(uint32_t now_ms,
                               FusionSnapshot *snapshot) const {
  if (snapshot == nullptr || !this->snapshot_.occupied ||
      time_reached(now_ms, this->snapshot_.expires_at_ms))
    return false;
  *snapshot = this->snapshot_;
  return true;
}

bool FusionSnapshotCache::expire(uint32_t now_ms) {
  if (!this->snapshot_.occupied ||
      !time_reached(now_ms, this->snapshot_.expires_at_ms))
    return false;
  this->clear();
  return true;
}

void FusionSnapshotCache::clear() {
  this->snapshot_ = {};
}

}  // namespace radar_api_server
}  // namespace esphome
