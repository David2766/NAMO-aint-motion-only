#pragma once

#include "device_config_cache.h"
#include "fixed_text_buffer.h"
#include "fusion_automation.h"
#include "presence_fusion.h"
#include "software_zone_evidence.h"
#include "shared_automation_area.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

struct StateJsonTarget {
  const char *id{""};
  const char *label{""};
  const char *color{""};
  float x{0.0f};
  float y{0.0f};
  bool active{false};
  bool fused{false};
  uint8_t source_count{0};
};

struct StateJsonLocalTarget {
  float x{0.0f};
  float y{0.0f};
  bool active{false};
};

static_assert(sizeof(StateJsonLocalTarget) <= 12,
              "local state target exceeded its embedded budget");

struct DeviceStateJsonInput {
  uint32_t now_ms{0};
  bool pir_motion{false};
  bool pir_motion_effective{false};
  bool filter_blocked{false};
  bool presence{false};
  bool motion{false};
  int target_count{0};
  int moving_target_count{0};
  int still_target_count{0};
  bool fusion_build_enabled{false};
  bool fusion_authoritative{false};
  const char *fusion_state{"disabled"};
  const FusionAutomationSnapshot *automation{nullptr};
  const SharedAutomationAreaCache *shared_automation_areas{nullptr};
  bool local_presence{false};
  bool local_motion{false};
  int local_target_count{0};
  float temperature_c{0.0f};
  float humidity_percent{0.0f};
  float illuminance_lux{0.0f};
  const char *presence_reason{"none"};
  const char *presence_off_reason{"none"};
  const char *motion_reason{"none"};
  uint32_t last_presence_drop_ms{0};
  uint32_t last_valid_target_age_ms{0};
  bool has_last_valid_target_age{false};
  int empty_samples_consecutive{0};
  uint32_t short_presence_drop_count{0};
  uint32_t long_presence_drop_count{0};
  const char *still_state{"idle"};
  const char *still_reason{"none"};
  int still_confidence{0};
  bool still_hold_active{false};
  uint32_t still_last_seen_age_ms{0};
  bool has_still_last_seen_age{false};
  bool still_anchor_valid{false};
  float still_anchor_x{0.0f};
  float still_anchor_y{0.0f};
  const char *range_reason{"ok"};
  int range_suspect_count{0};
  int range_out_of_range_count{0};
  int range_remote_candidate_count{0};
  StaticRadarEvidence static_radar{};
  const char *static_radar_reason{"unavailable"};
  bool pir_evidence{false};
  bool tracker_evidence{false};
  bool static_assist_armed{false};
  bool static_assist_active{false};
  bool static_assist_arm_pending{false};
  bool static_assist_exit_veto{false};
  uint32_t static_assist_arm_elapsed_ms{0};
  StaticAssistHeldTarget static_assist_held_target{};
  StateJsonLocalTarget local_targets[3]{};
  StateJsonTarget targets[3]{};
};

std::string build_device_state_json(const DeviceConfigCache &config_cache, const SoftwareZoneEvidence &zone_evidence,
                                    const std::string &tracker_debug_json, const DeviceStateJsonInput &input);

bool build_device_state_json(FixedTextBuffer &output,
                             const DeviceConfigCache &config_cache,
                             const SoftwareZoneEvidence &zone_evidence,
                             const char *tracker_debug_json,
                             const DeviceStateJsonInput &input);

}  // namespace radar_api_server
}  // namespace esphome
