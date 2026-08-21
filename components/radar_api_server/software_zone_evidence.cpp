#include "software_zone_evidence.h"

namespace esphome {
namespace radar_api_server {

namespace {

uint8_t containing_zone_index(const DeviceConfigCache &config_cache,
                              ZonePolicySource source, CachedZoneType type,
                              float target_x, float target_y) {
  const int count = source == ZonePolicySource::SOFTWARE
                        ? MAX_SOFTWARE_ZONES
                        : MAX_CALIBRATION_ZONES;
  for (int zone_index = 1; zone_index <= count; zone_index++) {
    const auto &zone = source == ZonePolicySource::SOFTWARE
                           ? config_cache.software_zone(zone_index)
                           : config_cache.calibration_zone(zone_index);
    if (zone.valid && zone.type == type && zone.contains(target_x, target_y))
      return static_cast<uint8_t>(zone_index);
  }
  return 0;
}

}  // namespace

int TargetZonePolicy::diagnostic_mode() const {
  if (this->source == ZonePolicySource::SOFTWARE &&
      this->action == ZonePolicyAction::BLOCKED)
    return 2;
  if (this->source == ZonePolicySource::SOFTWARE &&
      this->action == ZonePolicyAction::REDUCED)
    return 3;
  if (this->source == ZonePolicySource::CALIBRATION &&
      this->action == ZonePolicyAction::BLOCKED)
    return 10 + this->zone_index;
  if (this->source == ZonePolicySource::CALIBRATION &&
      this->action == ZonePolicyAction::REDUCED)
    return 20 + this->zone_index;
  return 0;
}

TargetZonePolicy compute_target_zone_policy(const DeviceConfigCache &config_cache,
                                            float target_x, float target_y) {
  const auto config = config_cache.read();
  const auto &snapshot = config.get();
  uint8_t zone_index = containing_zone_index(
      snapshot, ZonePolicySource::CALIBRATION, CachedZoneType::FILTER,
      target_x, target_y);
  if (zone_index != 0)
    return {ZonePolicyAction::BLOCKED, ZonePolicySource::CALIBRATION,
            zone_index};

  zone_index = containing_zone_index(
      snapshot, ZonePolicySource::SOFTWARE, CachedZoneType::FILTER,
      target_x, target_y);
  if (zone_index != 0)
    return {ZonePolicyAction::BLOCKED, ZonePolicySource::SOFTWARE, zone_index};

  zone_index = containing_zone_index(
      snapshot, ZonePolicySource::SOFTWARE, CachedZoneType::REDUCED,
      target_x, target_y);
  if (zone_index != 0)
    return {ZonePolicyAction::REDUCED, ZonePolicySource::SOFTWARE,
            zone_index};

  zone_index = containing_zone_index(
      snapshot, ZonePolicySource::CALIBRATION, CachedZoneType::REDUCED,
      target_x, target_y);
  if (zone_index != 0)
    return {ZonePolicyAction::REDUCED, ZonePolicySource::CALIBRATION,
            zone_index};

  return {};
}

SoftwareZoneEvidence compute_software_zone_evidence(const DeviceConfigCache &config_cache, uint32_t now_ms,
                                                    const SoftwareZoneTarget targets[3],
                                                    uint32_t *exit_zone_last_seen_ms) {
  const auto config = config_cache.read();
  const auto &snapshot = config.get();
  SoftwareZoneEvidence evidence;
  int target_exit_masks[3] = {0, 0, 0};

  for (int zone_index = 1; zone_index <= MAX_SOFTWARE_ZONES; zone_index++) {
    const auto &zone = snapshot.software_zone(zone_index);
    if (!zone.valid)
      continue;

    evidence.configured = true;
    const int zone_bit = 1 << (zone_index - 1);
    if (zone.type == CachedZoneType::DETECTION) {
      evidence.detection_zone_count++;
      for (int target_index = 0; target_index < 3; target_index++) {
        if (targets[target_index].valid &&
            zone.contains(targets[target_index].x, targets[target_index].y)) {
          evidence.detection_zone_counts[zone_index - 1]++;
          evidence.state_zone_counts[zone_index - 1]++;
          evidence.target_software_masks[target_index] |= zone_bit;
        }
      }
    } else if (zone.type == CachedZoneType::FILTER) {
      evidence.filter_zone_count++;
    } else if (zone.type == CachedZoneType::EXIT) {
      evidence.exit_zone_count++;
      for (int target_index = 0; target_index < 3; target_index++) {
        if (targets[target_index].valid &&
            zone.contains(targets[target_index].x, targets[target_index].y)) {
          evidence.state_zone_counts[zone_index - 1]++;
          target_exit_masks[target_index] |= zone_bit;
        }
      }
    }
  }

  evidence.target_exit_masks[0] = target_exit_masks[0];
  evidence.target_exit_masks[1] = target_exit_masks[1];
  evidence.target_exit_masks[2] = target_exit_masks[2];
  evidence.exit_zone_mask = target_exit_masks[0] | target_exit_masks[1] | target_exit_masks[2];
  evidence.exit_target_count = (target_exit_masks[0] != 0 ? 1 : 0) + (target_exit_masks[1] != 0 ? 1 : 0) +
                               (target_exit_masks[2] != 0 ? 1 : 0);
  evidence.exit_zone_active = evidence.exit_target_count > 0;
  if (exit_zone_last_seen_ms != nullptr) {
    if (evidence.exit_zone_active)
      *exit_zone_last_seen_ms = now_ms;
    evidence.exit_last_seen_age_ms =
        *exit_zone_last_seen_ms == 0 ? -1 : static_cast<int>(now_ms - *exit_zone_last_seen_ms);
  }

  const auto &room = snapshot.floorplan_room();
  evidence.room_context_configured = room.configured;
  evidence.room_boundary_point_count = room.boundary_point_count;
  if (room.boundary_point_count >= 3) {
    for (int target_index = 0; target_index < 3; target_index++) {
      if (!targets[target_index].valid)
        continue;
      evidence.target_room_signed_distances[target_index] =
          snapshot.floorplan_room_signed_distance(targets[target_index].x, targets[target_index].y);
      if (snapshot.point_in_floorplan_room(targets[target_index].x, targets[target_index].y)) {
        evidence.target_room_inside[target_index] = true;
        evidence.room_target_inside_count++;
      } else {
        evidence.target_room_outside[target_index] = true;
        evidence.room_target_outside_count++;
      }
    }
  }

  return evidence;
}

}  // namespace radar_api_server
}  // namespace esphome
