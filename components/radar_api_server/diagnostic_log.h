#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "presence_fusion.h"

namespace esphome {
namespace radar_api_server {

struct DiagnosticSnapshot {
  uint32_t ms{0};
  bool presence{false};
  bool motion{false};
  bool pir_motion{false};
  int target_count{0};
  int moving_target_count{0};
  int still_target_count{0};
  int still_confidence{0};
  bool still_hold_active{false};
  int empty_samples{0};
  int range_suspect_count{0};
  int range_out_of_range_count{0};
  int range_remote_candidate_count{0};
  float illuminance_lux{0.0f};
  const char *presence_reason{nullptr};
  const char *presence_off_reason{nullptr};
  const char *motion_reason{nullptr};
  const char *still_state{nullptr};
  const char *still_reason{nullptr};
  const char *range_reason{nullptr};
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
  const char *tracker_state{nullptr};
  const char *tracker_reason{nullptr};
  StaticRadarEvidence static_radar{};
  bool pir_evidence{false};
  bool tracker_evidence{false};
  bool static_assist_armed{false};
  bool static_assist_active{false};
  bool static_assist_arm_pending{false};
  bool static_assist_exit_veto{false};
  uint32_t static_assist_arm_elapsed_ms{0};
};

class DiagnosticLog {
 public:
  ~DiagnosticLog();

  void update(const DiagnosticSnapshot &snapshot);

  std::string to_text() const;
  void format_text_header(char *out, size_t out_size) const;
  bool format_text_event(size_t offset, char *out, size_t out_size) const;
  void format_json_header(char *out, size_t out_size) const;
  bool format_json_event(size_t offset, std::string *out) const;
  void format_json_footer(char *out, size_t out_size) const;

  size_t count() const { return this->events_ != nullptr ? this->count_ : 0; }
  bool truncated() const { return this->truncated_; }
  bool available() const { return !this->allocation_failed_; }

 private:
  struct Event {
    uint32_t sequence{0};
    uint32_t ms{0};
    char type[28]{};
    char reason[40]{};
    bool presence{false};
    bool motion{false};
    bool pir_motion{false};
    int target_count{0};
    int moving_target_count{0};
    int still_target_count{0};
    int still_confidence{0};
    bool still_hold_active{false};
    int empty_samples{0};
    int range_suspect_count{0};
    int range_out_of_range_count{0};
    int range_remote_candidate_count{0};
    float illuminance_lux{0.0f};
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
    char tracker_state[20]{};
    char tracker_reason[40]{};
    char still_state[20]{};
    char still_reason[40]{};
    char range_reason[32]{};
    bool exit_zone_active{false};
    int exit_zone_mask{0};
    int exit_target_count{0};
    int exit_last_seen_age_ms{-1};
  };

  static constexpr size_t MAX_EVENTS = 256;
  static constexpr uint32_t RETENTION_MS = 24UL * 60UL * 60UL * 1000UL;

  Event *events_{nullptr};
  size_t start_{0};
  size_t count_{0};
  uint32_t next_sequence_{1};
  bool truncated_{false};
  bool allocation_failed_{false};
  bool has_previous_{false};
  DiagnosticSnapshot previous_{};
  std::string previous_still_state_;
  std::string previous_still_reason_;
  std::string previous_range_reason_;
  std::string previous_tracker_state_;
  std::string previous_tracker_reason_;

  bool ensure_allocated_();
  void append_event_(const char *type, const char *reason, const DiagnosticSnapshot &snapshot);
  void trim_(uint32_t now_ms);
  const Event &event_at_(size_t offset) const;
  static void copy_string_(char *dst, size_t dst_size, const char *src);
  static const char *safe_(const char *value);
  static void append_json_string_(std::string &out, const char *value);
};

}  // namespace radar_api_server
}  // namespace esphome
