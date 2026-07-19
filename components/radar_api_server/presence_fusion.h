#pragma once

#include "presence_tracker.h"

namespace esphome {
namespace radar_api_server {

struct StaticRadarEvidence {
  bool available{false};
  bool presence{false};
  bool moving{false};
  bool still{false};
  float detection_distance_mm{0.0f};
  float moving_distance_mm{0.0f};
  float still_distance_mm{0.0f};
  float moving_energy{0.0f};
  float still_energy{0.0f};
};

struct PresenceFusionInput {
  uint32_t now_ms{0};
  bool pir_motion{false};
  bool filter_blocked{false};
  bool stable_presence{false};
  StaticRadarEvidence static_radar{};
};

struct StaticAssistHeldTarget {
  bool valid{false};
  bool distance_comparable{false};
  bool distance_matched{false};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float last_distance_mm{0.0f};
  float static_distance_mm{0.0f};
  float distance_delta_mm{0.0f};
};

struct PresenceFusionOutput {
  bool presence{false};
  bool motion{false};
  const char *presence_reason{"none"};
  const char *presence_off_reason{"tracker_lost_hold_expired"};
  const char *motion_reason{"none"};
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
};

class PresenceFusion {
 public:
  void update(const PresenceFusionInput &input, const PresenceTrackerOutput &tracker);

  const PresenceFusionOutput &output() const { return this->output_; }

 private:
  static constexpr uint32_t STATIC_ASSIST_ARM_MS = 2000;
  static constexpr uint32_t STATIC_ASSIST_REENTRY_MS = 30000;
  static constexpr uint32_t STATIC_DISTANCE_SAMPLE_MS = 1000;
  static constexpr float STATIC_ASSIST_TARGET_DISTANCE_TOLERANCE_MM = 750.0f;

  PresenceFusionOutput output_{};
  bool static_assist_armed_{false};
  bool static_assist_arm_pending_{false};
  bool static_assist_exit_veto_{false};
  bool static_assist_reentry_pending_{false};
  const char *static_assist_exit_reason_{"none"};
  bool previous_pir_motion_{false};
  bool previous_stable_presence_{false};
  uint32_t static_assist_arm_started_ms_{0};
  uint32_t static_assist_reentry_started_ms_{0};
  uint32_t last_processed_drop_ms_{0};
  bool last_spatial_target_valid_{false};
  float last_spatial_target_x_mm_{0.0f};
  float last_spatial_target_y_mm_{0.0f};
  float last_spatial_target_distance_mm_{0.0f};
  float static_distance_samples_[3]{0.0f, 0.0f, 0.0f};
  float stable_static_distance_mm_{0.0f};
  uint32_t static_distance_last_sample_ms_{0};
  uint8_t static_distance_sample_count_{0};
  uint8_t static_distance_next_sample_{0};

  static bool has_drop_reason_(const char *value);
  static bool is_exit_drop_reason_(const char *value);
  static float static_radar_distance_mm_(const StaticRadarEvidence &evidence);
  void update_static_distance_(uint32_t now_ms, const StaticRadarEvidence &evidence);
  void reset_static_distance_();
  void update_last_spatial_target_(const PresenceTrackerOutput &tracker);
  void reset_session_();
};

}  // namespace radar_api_server
}  // namespace esphome
