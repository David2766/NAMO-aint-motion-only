#pragma once

#include "device_config_cache.h"

#include <cstdint>

namespace esphome {
namespace radar_api_server {

struct SoftwareZoneTarget {
  bool valid{false};
  float x{0};
  float y{0};
};

enum class ZonePolicyAction : uint8_t {
  NONE = 0,
  BLOCKED,
  REDUCED,
};

enum class ZonePolicySource : uint8_t {
  NONE = 0,
  SOFTWARE,
  CALIBRATION,
};

struct TargetZonePolicy {
  ZonePolicyAction action{ZonePolicyAction::NONE};
  ZonePolicySource source{ZonePolicySource::NONE};
  uint8_t zone_index{0};

  bool filtered() const { return this->action != ZonePolicyAction::NONE; }
  bool filter_blocked() const { return this->action == ZonePolicyAction::BLOCKED; }
  bool reduced() const { return this->action == ZonePolicyAction::REDUCED; }
  int diagnostic_mode() const;
};

struct SoftwareZoneEvidence {
  bool configured{false};
  int detection_zone_count{0};
  int filter_zone_count{0};
  int exit_zone_count{0};
  int detection_zone_counts[6]{0, 0, 0, 0, 0, 0};
  int state_zone_counts[6]{0, 0, 0, 0, 0, 0};
  int target_software_masks[3]{0, 0, 0};
  int target_exit_masks[3]{0, 0, 0};
  bool target_room_inside[3]{false, false, false};
  bool target_room_outside[3]{false, false, false};
  float target_room_signed_distances[3]{0.0f, 0.0f, 0.0f};
  bool exit_zone_active{false};
  int exit_zone_mask{0};
  int exit_target_count{0};
  int exit_last_seen_age_ms{-1};
  bool room_context_configured{false};
  int room_boundary_point_count{0};
  int room_target_inside_count{0};
  int room_target_outside_count{0};
};

TargetZonePolicy compute_target_zone_policy(const DeviceConfigCache &config_cache,
                                            float target_x, float target_y);
SoftwareZoneEvidence compute_software_zone_evidence(const DeviceConfigCache &config_cache, uint32_t now_ms,
                                                    const SoftwareZoneTarget targets[3],
                                                    uint32_t *exit_zone_last_seen_ms);

}  // namespace radar_api_server
}  // namespace esphome
