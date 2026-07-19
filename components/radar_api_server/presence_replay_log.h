#pragma once

#include "diagnostic_log.h"
#include "presence_tracker.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

struct PresenceReplayTargetInput {
  bool valid{false};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float speed_cm_s{0.0f};
  float distance_mm{0.0f};
  int filter_mode{0};
  bool filtered{false};
  bool range_valid{false};
};

struct PresenceReplayRawInput {
  std::array<PresenceReplayTargetInput, 3> targets{};
};

class PresenceReplayLog {
 public:
  ~PresenceReplayLog();

  void update(const PresenceTrackerInput &input, const PresenceReplayRawInput &raw_input,
              const DiagnosticSnapshot &snapshot);

  size_t count() const { return this->samples_ != nullptr ? this->count_ : 0; }
  bool truncated() const { return this->truncated_; }
  bool available() const { return !this->allocation_failed_; }
  bool format_ndjson_sample(size_t offset, char *out, size_t out_size) const;

 private:
  struct Target {
    bool valid{false};
    int16_t x_mm{0};
    int16_t y_mm{0};
    int16_t speed_cm_s{0};
    uint16_t distance_mm{0};
    int16_t filter_mode{0};
    bool filtered{false};
    bool range_valid{false};
  };

  struct Sample {
    uint32_t sequence{0};
    uint32_t ms{0};
    bool pir_motion{false};
    bool filter_blocked{false};
    float illuminance_lux{0.0f};
    std::array<Target, 3> raw_targets{};
    std::array<Target, 3> targets{};
    bool presence{false};
    bool motion{false};
    bool still{false};
    int still_confidence{0};
    int target_count{0};
    int moving_target_count{0};
    int still_target_count{0};
    int empty_samples{0};
    int range_suspect_count{0};
    int range_out_of_range_count{0};
    int range_remote_candidate_count{0};
    bool exit_zone_active{false};
    int exit_zone_mask{0};
    int exit_target_count{0};
    int exit_last_seen_age_ms{-1};
    bool tracker_presence{false};
    bool tracker_motion{false};
    int tracker_track_score{0};
    int tracker_input_detection_count{0};
    int tracker_active_track_count{0};
    int tracker_tentative_track_count{0};
    int tracker_confirmed_track_count{0};
    int tracker_coasting_track_count{0};
    int tracker_moving_track_count{0};
    int tracker_still_track_count{0};
    bool static_radar_available{false};
    bool static_radar_presence{false};
    bool static_radar_moving{false};
    bool static_radar_still{false};
    uint16_t static_radar_moving_distance_mm{0};
    uint16_t static_radar_still_distance_mm{0};
    uint16_t static_radar_moving_energy{0};
    uint16_t static_radar_still_energy{0};
    uint16_t static_radar_detection_distance_mm{0};
    bool pir_evidence{false};
    bool tracker_evidence{false};
    bool static_assist_armed{false};
    bool static_assist_active{false};
    bool static_assist_arm_pending{false};
    bool static_assist_exit_veto{false};
    uint16_t static_assist_arm_elapsed_ms{0};
    uint8_t presence_reason_code{0};
    uint8_t presence_off_reason_code{0};
    uint8_t motion_reason_code{0};
    uint8_t still_state_code{0};
    uint8_t still_reason_code{0};
    uint8_t range_reason_code{0};
    uint8_t tracker_state_code{0};
    uint8_t tracker_reason_code{0};
  };

  static constexpr size_t MAX_SAMPLES = 1200;

  Sample *samples_{nullptr};
  size_t start_{0};
  size_t count_{0};
  uint32_t next_sequence_{1};
  bool truncated_{false};
  bool allocation_failed_{false};

  bool ensure_allocated_();
  const Sample &sample_at_(size_t offset) const;
  static int16_t clamp_i16_(float value);
  static uint16_t clamp_u16_(float value);
  static const char *safe_(const char *value);
  static uint8_t presence_reason_code_(const char *value);
  static uint8_t motion_reason_code_(const char *value);
  static uint8_t still_state_code_(const char *value);
  static uint8_t still_reason_code_(const char *value);
  static uint8_t range_reason_code_(const char *value);
  static uint8_t tracker_state_code_(const char *value);
  static uint8_t tracker_reason_code_(const char *value);
};

}  // namespace radar_api_server
}  // namespace esphome
