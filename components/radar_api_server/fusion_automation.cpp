#include "fusion_automation.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

bool valid_text(const char *value) {
  return value != nullptr && value[0] != '\0';
}

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

template<std::size_t N>
void copy_text(char (&target)[N], const char *source) {
  std::strncpy(target, source != nullptr ? source : "", N - 1);
  target[N - 1] = '\0';
}

bool valid_remote_snapshot(const FusionSnapshot &snapshot) {
  if (!snapshot.occupied || !valid_text(snapshot.site_id) ||
      !valid_text(snapshot.leader_node_id) ||
      !valid_text(snapshot.group_id) || !valid_text(snapshot.boot_id) ||
      snapshot.sequence == 0 || snapshot.group_term == 0 ||
      snapshot.source_sequence == 0 ||
      snapshot.ttl_ms < FusionSnapshot::MIN_TTL_MS ||
      snapshot.ttl_ms > FusionSnapshot::MAX_TTL_MS ||
      snapshot.target_count > snapshot.targets.size() ||
      snapshot.presence != (snapshot.target_count > 0))
    return false;
  uint8_t valid_count = 0;
  for (const auto &target : snapshot.targets) {
    if (!target.valid)
      continue;
    if (target.fused_track_id == 0 || target.source_count == 0 ||
        target.confidence > 100 ||
        (target.state != SpatialShadowTrackState::CONFIRMED &&
         target.state != SpatialShadowTrackState::COASTING) ||
        !std::isfinite(target.x_mm) || !std::isfinite(target.y_mm) ||
        !std::isfinite(target.vx_mm_s) || !std::isfinite(target.vy_mm_s))
      return false;
    ++valid_count;
  }
  return valid_count == snapshot.target_count;
}

}  // namespace

const char *fusion_automation_health_to_string(FusionAutomationHealth health) {
  switch (health) {
    case FusionAutomationHealth::DISABLED: return "disabled";
    case FusionAutomationHealth::UNAVAILABLE: return "unavailable";
    case FusionAutomationHealth::DEGRADED: return "degraded";
    case FusionAutomationHealth::RECOVERING: return "recovering";
    case FusionAutomationHealth::HEALTHY: return "healthy";
  }
  return "unavailable";
}

const char *fusion_automation_source_to_string(FusionAutomationSource source) {
  switch (source) {
    case FusionAutomationSource::NONE: return "none";
    case FusionAutomationSource::LOCAL_GROUP_LEADER:
      return "local_group_leader";
    case FusionAutomationSource::GROUP_LEADER_SNAPSHOT:
      return "group_leader_snapshot";
  }
  return "none";
}

void FusionAutomationState::disable(const char *site_id, const char *space_id,
                                    const char *group_id,
                                    const char *leader_node_id,
                                    uint32_t group_term) {
  this->snapshot_ = {};
  copy_text(this->snapshot_.site_id, site_id);
  copy_text(this->snapshot_.space_id, space_id);
  copy_text(this->snapshot_.group_id, group_id);
  copy_text(this->snapshot_.leader_node_id, leader_node_id);
  this->snapshot_.group_term = group_term;
}

void FusionAutomationState::mark_unavailable(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, uint32_t group_term, bool degraded) {
  const bool same_authority =
      this->snapshot_.sequence != 0 &&
      std::strcmp(this->snapshot_.site_id,
                  site_id != nullptr ? site_id : "") == 0 &&
      std::strcmp(this->snapshot_.group_id,
                  group_id != nullptr ? group_id : "") == 0 &&
      std::strcmp(this->snapshot_.leader_node_id,
                  leader_node_id != nullptr ? leader_node_id : "") == 0 &&
      this->snapshot_.group_term == group_term;
  FusionAutomationSnapshot unavailable =
      same_authority ? this->snapshot_ : FusionAutomationSnapshot{};
  unavailable.build_enabled = true;
  unavailable.authoritative = false;
  unavailable.health = degraded ? FusionAutomationHealth::DEGRADED
                                : FusionAutomationHealth::UNAVAILABLE;
  copy_text(unavailable.site_id, site_id);
  copy_text(unavailable.space_id, space_id);
  copy_text(unavailable.group_id, group_id);
  copy_text(unavailable.leader_node_id, leader_node_id);
  unavailable.group_term = group_term;
  unavailable.continuity_held = false;
  unavailable.continuity_source_leader_node_id[0] = '\0';
  unavailable.continuity_source_group_term = 0;
  unavailable.continuity_started_at_ms = 0;
  unavailable.continuity_expires_at_ms = 0;
  unavailable.presence = false;
  unavailable.motion = false;
  unavailable.fusion = {};
  unavailable.fusion.build_enabled = true;
  unavailable.fusion.state = SpatialProductionFusionState::DEGRADED;
  this->snapshot_ = unavailable;
}

bool FusionAutomationState::mark_recovering(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, uint32_t group_term, uint32_t now_ms) {
  const bool same_logical_space =
      this->snapshot_.sequence != 0 && valid_text(site_id) &&
      valid_text(space_id) && valid_text(group_id) &&
      std::strcmp(this->snapshot_.site_id, site_id) == 0 &&
      std::strcmp(this->snapshot_.space_id, space_id) == 0 &&
      std::strcmp(this->snapshot_.group_id, group_id) == 0;
  if (!same_logical_space ||
      (this->snapshot_.health != FusionAutomationHealth::HEALTHY &&
       this->snapshot_.health != FusionAutomationHealth::RECOVERING))
    return false;

  FusionAutomationSnapshot next = this->snapshot_;
  if (!next.continuity_held) {
    next.continuity_held = true;
    copy_text(next.continuity_source_leader_node_id, next.leader_node_id);
    next.continuity_source_group_term = next.group_term;
    next.continuity_started_at_ms = now_ms;
    next.continuity_expires_at_ms = now_ms + CONTINUITY_HOLD_MS;
  }
  if (time_reached(now_ms, next.continuity_expires_at_ms) ||
      (group_term != 0 &&
       group_term < next.continuity_source_group_term))
    return false;

  next.build_enabled = true;
  next.authoritative = false;
  next.health = FusionAutomationHealth::RECOVERING;
  next.motion = false;
  next.fusion.authoritative = false;
  next.fusion.state = SpatialProductionFusionState::DEGRADED;
  if (valid_text(leader_node_id) && group_term != 0) {
    copy_text(next.leader_node_id, leader_node_id);
    next.group_term = group_term;
  }
  this->snapshot_ = next;
  return true;
}

bool FusionAutomationState::preserve_continuity_or_mark_unavailable(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, uint32_t group_term, uint32_t now_ms,
    bool degraded) {
  if (this->mark_recovering(site_id, space_id, group_id, leader_node_id,
                            group_term, now_ms))
    return true;
  this->mark_unavailable(site_id, space_id, group_id, leader_node_id,
                         group_term, degraded);
  return false;
}

uint32_t FusionAutomationState::continuity_age_ms(
    const FusionAutomationSnapshot &snapshot, uint32_t now_ms) {
  if (!snapshot.continuity_held || snapshot.continuity_started_at_ms == 0)
    return 0;
  return now_ms - snapshot.continuity_started_at_ms;
}

uint32_t FusionAutomationState::continuity_remaining_ms(
    const FusionAutomationSnapshot &snapshot, uint32_t now_ms) {
  if (!snapshot.continuity_held || snapshot.continuity_expires_at_ms == 0 ||
      time_reached(now_ms, snapshot.continuity_expires_at_ms))
    return 0;
  return snapshot.continuity_expires_at_ms - now_ms;
}

bool FusionAutomationState::update_local(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, const char *boot_id, uint32_t group_term,
    const SpatialProductionFusionOutput &fusion, uint32_t now_ms) {
  if (!valid_text(site_id) || !valid_text(space_id) ||
      !valid_text(group_id) || !valid_text(leader_node_id) ||
      !valid_text(boot_id) ||
      group_term == 0 || !fusion.build_enabled || !fusion.authoritative ||
      fusion.state != SpatialProductionFusionState::ACTIVE ||
      fusion.source_sequence == 0)
    return false;
  const bool same_authority =
      this->snapshot_.sequence != 0 &&
      std::strcmp(this->snapshot_.site_id, site_id) == 0 &&
      std::strcmp(this->snapshot_.group_id, group_id) == 0 &&
      std::strcmp(this->snapshot_.leader_node_id, leader_node_id) == 0 &&
      std::strcmp(this->snapshot_.boot_id, boot_id) == 0 &&
      this->snapshot_.group_term == group_term;
  if (same_authority && fusion.source_sequence < this->snapshot_.sequence)
    return false;
  if (same_authority && fusion.source_sequence == this->snapshot_.sequence &&
      this->snapshot_.authoritative)
    return true;

  FusionAutomationSnapshot next{};
  next.build_enabled = true;
  next.authoritative = true;
  next.health = FusionAutomationHealth::HEALTHY;
  next.source = FusionAutomationSource::LOCAL_GROUP_LEADER;
  copy_text(next.site_id, site_id);
  copy_text(next.space_id, space_id);
  copy_text(next.group_id, group_id);
  copy_text(next.leader_node_id, leader_node_id);
  copy_text(next.boot_id, boot_id);
  next.group_term = group_term;
  next.sequence = fusion.source_sequence;
  next.source_sequence = fusion.source_sequence;
  next.updated_at_ms = now_ms;
  next.presence = fusion.target_count > 0;
  next.motion = false;
  next.fusion = fusion;
  for (const auto &target : fusion.targets) {
    if (target.valid && std::hypot(target.vx_mm_s, target.vy_mm_s) > 100.0f) {
      next.motion = true;
      break;
    }
  }
  this->snapshot_ = next;
  return true;
}

bool FusionAutomationState::update_remote(
    const FusionSnapshot &snapshot, const char *space_id,
    const SpatialTransform2D &local_transform, bool authority_admitted) {
  if (!valid_remote_snapshot(snapshot) || !valid_text(space_id) ||
      !valid_spatial_transform(local_transform))
    return false;
  if (!authority_admitted && this->snapshot_.sequence != 0 &&
      std::strcmp(this->snapshot_.site_id, snapshot.site_id) == 0 &&
      std::strcmp(this->snapshot_.group_id, snapshot.group_id) == 0) {
    if (snapshot.group_term < this->snapshot_.group_term)
      return false;
    if (snapshot.group_term == this->snapshot_.group_term &&
        std::strcmp(this->snapshot_.leader_node_id,
                    snapshot.leader_node_id) != 0)
      return false;
    if (snapshot.group_term == this->snapshot_.group_term &&
        std::strcmp(this->snapshot_.leader_node_id,
                    snapshot.leader_node_id) == 0 &&
        std::strcmp(this->snapshot_.boot_id, snapshot.boot_id) == 0 &&
        snapshot.sequence < this->snapshot_.sequence)
      return false;
  }
  if (this->snapshot_.authoritative &&
      this->snapshot_.group_term == snapshot.group_term &&
      std::strcmp(this->snapshot_.leader_node_id,
                  snapshot.leader_node_id) == 0 &&
      std::strcmp(this->snapshot_.boot_id, snapshot.boot_id) == 0 &&
      this->snapshot_.sequence == snapshot.sequence)
    return true;

  FusionAutomationSnapshot next{};
  next.build_enabled = true;
  next.authoritative = true;
  next.health = FusionAutomationHealth::HEALTHY;
  next.source = FusionAutomationSource::GROUP_LEADER_SNAPSHOT;
  copy_text(next.site_id, snapshot.site_id);
  copy_text(next.space_id, space_id);
  copy_text(next.group_id, snapshot.group_id);
  copy_text(next.leader_node_id, snapshot.leader_node_id);
  copy_text(next.boot_id, snapshot.boot_id);
  next.group_term = snapshot.group_term;
  next.sequence = snapshot.sequence;
  next.source_sequence = snapshot.source_sequence;
  next.updated_at_ms = snapshot.received_at_ms;
  next.expires_at_ms = snapshot.expires_at_ms;
  next.presence = snapshot.presence;
  next.motion = snapshot.motion;
  next.fusion.build_enabled = true;
  next.fusion.authoritative = true;
  next.fusion.state = SpatialProductionFusionState::ACTIVE;
  next.fusion.source_sequence = snapshot.source_sequence;
  next.fusion.target_count = snapshot.target_count;
  for (std::size_t index = 0; index < snapshot.targets.size(); ++index) {
    const auto &source = snapshot.targets[index];
    if (!source.valid)
      continue;
    const auto point = apply_inverse_spatial_transform(
        local_transform, source.x_mm, source.y_mm);
    const auto velocity = apply_inverse_spatial_vector(
        local_transform, source.vx_mm_s, source.vy_mm_s);
    auto &target = next.fusion.targets[index];
    target.valid = true;
    target.fused_track_id = source.fused_track_id;
    target.state = source.state;
    target.source_count = source.source_count;
    target.confidence = source.confidence;
    target.x_mm = point.x;
    target.y_mm = point.y;
    target.vx_mm_s = velocity.x;
    target.vy_mm_s = velocity.y;
  }
  this->snapshot_ = next;
  return true;
}

}  // namespace radar_api_server
}  // namespace esphome
