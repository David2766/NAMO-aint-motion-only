#include "presence_replay_log.h"

#include <esp_heap_caps.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace radar_api_server {

PresenceReplayLog::~PresenceReplayLog() {
  if (this->samples_ != nullptr) {
    heap_caps_free(this->samples_);
    this->samples_ = nullptr;
  }
}

void PresenceReplayLog::update(const PresenceTrackerInput &input, const PresenceReplayRawInput &raw_input,
                               const DiagnosticSnapshot &snapshot) {
  if (!this->ensure_allocated_())
    return;

  const size_t index = (this->start_ + this->count_) % MAX_SAMPLES;
  Sample &sample = this->samples_[index];

  sample.sequence = this->next_sequence_++;
  sample.ms = snapshot.ms;
  sample.pir_motion = input.pir_motion;
  sample.filter_blocked = input.filter_blocked;
  sample.illuminance_lux = input.illuminance_lux;

  for (size_t i = 0; i < sample.raw_targets.size(); i++) {
    sample.raw_targets[i].valid = raw_input.targets[i].valid;
    sample.raw_targets[i].x_mm = clamp_i16_(raw_input.targets[i].x_mm);
    sample.raw_targets[i].y_mm = clamp_i16_(raw_input.targets[i].y_mm);
    sample.raw_targets[i].speed_cm_s = clamp_i16_(raw_input.targets[i].speed_cm_s);
    sample.raw_targets[i].distance_mm = clamp_u16_(raw_input.targets[i].distance_mm);
    sample.raw_targets[i].filter_mode = clamp_i16_(static_cast<float>(raw_input.targets[i].filter_mode));
    sample.raw_targets[i].filtered = raw_input.targets[i].filtered;
    sample.raw_targets[i].range_valid = raw_input.targets[i].range_valid;
  }

  for (size_t i = 0; i < sample.targets.size(); i++) {
    sample.targets[i].valid = input.targets[i].valid;
    sample.targets[i].x_mm = clamp_i16_(input.targets[i].x_mm);
    sample.targets[i].y_mm = clamp_i16_(input.targets[i].y_mm);
    sample.targets[i].speed_cm_s = clamp_i16_(input.targets[i].speed_cm_s);
    sample.targets[i].distance_mm = clamp_u16_(input.targets[i].distance_mm);
  }

  sample.presence = snapshot.presence;
  sample.motion = snapshot.motion;
  sample.still = snapshot.still_target_count > 0;
  sample.still_confidence = snapshot.still_confidence;
  sample.target_count = snapshot.target_count;
  sample.moving_target_count = snapshot.moving_target_count;
  sample.still_target_count = snapshot.still_target_count;
  sample.empty_samples = snapshot.empty_samples;
  sample.range_suspect_count = snapshot.range_suspect_count;
  sample.range_out_of_range_count = snapshot.range_out_of_range_count;
  sample.range_remote_candidate_count = snapshot.range_remote_candidate_count;
  sample.exit_zone_active = snapshot.exit_zone_active;
  sample.exit_zone_mask = snapshot.exit_zone_mask;
  sample.exit_target_count = snapshot.exit_target_count;
  sample.exit_last_seen_age_ms = snapshot.exit_last_seen_age_ms;
  sample.tracker_presence = snapshot.tracker_presence;
  sample.tracker_motion = snapshot.tracker_motion;
  sample.tracker_track_score = snapshot.tracker_track_score;
  sample.tracker_input_detection_count = snapshot.tracker_input_detection_count;
  sample.tracker_active_track_count = snapshot.tracker_active_track_count;
  sample.tracker_tentative_track_count = snapshot.tracker_tentative_track_count;
  sample.tracker_confirmed_track_count = snapshot.tracker_confirmed_track_count;
  sample.tracker_coasting_track_count = snapshot.tracker_coasting_track_count;
  sample.tracker_moving_track_count = snapshot.tracker_moving_track_count;
  sample.tracker_still_track_count = snapshot.tracker_still_track_count;
  sample.static_radar_available = snapshot.static_radar.available;
  sample.static_radar_presence = snapshot.static_radar.presence;
  sample.static_radar_moving = snapshot.static_radar.moving;
  sample.static_radar_still = snapshot.static_radar.still;
  sample.static_radar_moving_distance_mm = clamp_u16_(snapshot.static_radar.moving_distance_mm);
  sample.static_radar_still_distance_mm = clamp_u16_(snapshot.static_radar.still_distance_mm);
  sample.static_radar_moving_energy = clamp_u16_(snapshot.static_radar.moving_energy);
  sample.static_radar_still_energy = clamp_u16_(snapshot.static_radar.still_energy);
  sample.static_radar_detection_distance_mm = clamp_u16_(snapshot.static_radar.detection_distance_mm);
  sample.pir_evidence = snapshot.pir_evidence;
  sample.tracker_evidence = snapshot.tracker_evidence;
  sample.static_assist_armed = snapshot.static_assist_armed;
  sample.static_assist_active = snapshot.static_assist_active;
  sample.static_assist_arm_pending = snapshot.static_assist_arm_pending;
  sample.static_assist_exit_veto = snapshot.static_assist_exit_veto;
  sample.static_assist_arm_elapsed_ms = clamp_u16_(snapshot.static_assist_arm_elapsed_ms);
  sample.presence_reason_code = presence_reason_code_(snapshot.presence_reason);
  sample.presence_off_reason_code = presence_reason_code_(snapshot.presence_off_reason);
  sample.motion_reason_code = motion_reason_code_(snapshot.motion_reason);
  sample.still_state_code = still_state_code_(snapshot.still_state);
  sample.still_reason_code = still_reason_code_(snapshot.still_reason);
  sample.range_reason_code = range_reason_code_(snapshot.range_reason);
  sample.tracker_state_code = tracker_state_code_(snapshot.tracker_state);
  sample.tracker_reason_code = tracker_reason_code_(snapshot.tracker_reason);

  if (this->count_ < MAX_SAMPLES) {
    this->count_++;
  } else {
    this->start_ = (this->start_ + 1) % MAX_SAMPLES;
    this->truncated_ = true;
  }
}

bool PresenceReplayLog::format_ndjson_sample(size_t offset, char *out, size_t out_size) const {
  if (out == nullptr || out_size == 0 || offset >= this->count_)
    return false;
  if (this->samples_ == nullptr)
    return false;

  const Sample &sample = this->sample_at_(offset);
  const int written = std::snprintf(
      out, out_size,
      "{\"q\":%u,\"t\":%u,\"p\":%u,\"lx\":%.1f,"
      "\"r\":[[%u,%d,%d,%d,%u,%d,%u,%u],[%u,%d,%d,%d,%u,%d,%u,%u],[%u,%d,%d,%d,%u,%d,%u,%u]],"
      "\"tg\":[[%u,%d,%d,%d,%u],[%u,%d,%d,%d,%u],[%u,%d,%d,%d,%u]],"
      "\"sr\":[%u,%u,%u,%u,%u,%u,%u,%u,%u],"
      "\"sf\":[%u,%u,%u,%u,%u,%u,%u],"
      "\"f\":[%u,%u,%d,%d,%d],"
      "\"ex\":[%u,%d,%d,%d],"
      "\"l\":[%u,%u,%u,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u],"
      "\"tr\":[%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d]}\n",
      sample.sequence, sample.ms, sample.pir_motion ? 1 : 0, sample.illuminance_lux,
      sample.raw_targets[0].valid ? 1 : 0, sample.raw_targets[0].x_mm, sample.raw_targets[0].y_mm,
      sample.raw_targets[0].speed_cm_s, sample.raw_targets[0].distance_mm, sample.raw_targets[0].filter_mode,
      sample.raw_targets[0].filtered ? 1 : 0, sample.raw_targets[0].range_valid ? 1 : 0,
      sample.raw_targets[1].valid ? 1 : 0, sample.raw_targets[1].x_mm, sample.raw_targets[1].y_mm,
      sample.raw_targets[1].speed_cm_s, sample.raw_targets[1].distance_mm, sample.raw_targets[1].filter_mode,
      sample.raw_targets[1].filtered ? 1 : 0, sample.raw_targets[1].range_valid ? 1 : 0,
      sample.raw_targets[2].valid ? 1 : 0, sample.raw_targets[2].x_mm, sample.raw_targets[2].y_mm,
      sample.raw_targets[2].speed_cm_s, sample.raw_targets[2].distance_mm, sample.raw_targets[2].filter_mode,
      sample.raw_targets[2].filtered ? 1 : 0, sample.raw_targets[2].range_valid ? 1 : 0,
      sample.targets[0].valid ? 1 : 0, sample.targets[0].x_mm, sample.targets[0].y_mm,
      sample.targets[0].speed_cm_s, sample.targets[0].distance_mm,
      sample.targets[1].valid ? 1 : 0, sample.targets[1].x_mm, sample.targets[1].y_mm,
      sample.targets[1].speed_cm_s, sample.targets[1].distance_mm,
      sample.targets[2].valid ? 1 : 0, sample.targets[2].x_mm, sample.targets[2].y_mm,
      sample.targets[2].speed_cm_s, sample.targets[2].distance_mm,
      sample.static_radar_available ? 1 : 0, sample.static_radar_presence ? 1 : 0,
      sample.static_radar_moving ? 1 : 0, sample.static_radar_still ? 1 : 0,
      sample.static_radar_moving_distance_mm, sample.static_radar_still_distance_mm,
      sample.static_radar_moving_energy, sample.static_radar_still_energy,
      sample.static_radar_detection_distance_mm,
      sample.pir_evidence ? 1 : 0, sample.tracker_evidence ? 1 : 0,
      sample.static_assist_armed ? 1 : 0, sample.static_assist_active ? 1 : 0,
      sample.static_assist_arm_pending ? 1 : 0, sample.static_assist_exit_veto ? 1 : 0,
      sample.static_assist_arm_elapsed_ms,
      sample.filter_blocked ? 1 : 0,
      sample.range_reason_code,
      sample.range_suspect_count, sample.range_out_of_range_count, sample.range_remote_candidate_count,
      sample.exit_zone_active ? 1 : 0, sample.exit_zone_mask, sample.exit_target_count,
      sample.exit_last_seen_age_ms,
      sample.presence ? 1 : 0, sample.motion ? 1 : 0, sample.still ? 1 : 0, sample.target_count,
      sample.moving_target_count, sample.still_target_count, sample.still_confidence, sample.empty_samples,
      sample.presence_reason_code, sample.presence_off_reason_code, sample.motion_reason_code,
      sample.still_state_code, sample.still_reason_code, sample.tracker_presence ? 1 : 0,
      sample.tracker_motion ? 1 : 0, sample.tracker_state_code, sample.tracker_reason_code, sample.tracker_track_score,
      sample.tracker_input_detection_count, sample.tracker_active_track_count, sample.tracker_tentative_track_count,
      sample.tracker_confirmed_track_count, sample.tracker_coasting_track_count, sample.tracker_moving_track_count,
      sample.tracker_still_track_count);
  return written > 0 && static_cast<size_t>(written) < out_size;
}

bool PresenceReplayLog::ensure_allocated_() {
  if (this->samples_ != nullptr)
    return true;
  if (this->allocation_failed_)
    return false;

  this->samples_ = static_cast<Sample *>(heap_caps_calloc(MAX_SAMPLES, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->samples_ == nullptr) {
    this->count_ = 0;
    this->start_ = 0;
    this->truncated_ = false;
    this->allocation_failed_ = true;
    return false;
  }
  return true;
}

const PresenceReplayLog::Sample &PresenceReplayLog::sample_at_(size_t offset) const {
  return this->samples_[(this->start_ + offset) % MAX_SAMPLES];
}

int16_t PresenceReplayLog::clamp_i16_(float value) {
  if (!std::isfinite(value))
    return 0;
  if (value > 32767.0f)
    return 32767;
  if (value < -32768.0f)
    return -32768;
  return static_cast<int16_t>(std::lround(value));
}

uint16_t PresenceReplayLog::clamp_u16_(float value) {
  if (!std::isfinite(value) || value <= 0.0f)
    return 0;
  if (value > 65535.0f)
    return 65535;
  return static_cast<uint16_t>(std::lround(value));
}

const char *PresenceReplayLog::safe_(const char *value) {
  return value != nullptr && value[0] != '\0' ? value : "none";
}

uint8_t PresenceReplayLog::presence_reason_code_(const char *value) {
  const char *reason = safe_(value);
  if (std::strcmp(reason, "none") == 0) return 0;
  if (std::strcmp(reason, "target") == 0) return 1;
  if (std::strcmp(reason, "still_hold") == 0) return 2;
  if (std::strcmp(reason, "pir") == 0) return 3;
  if (std::strcmp(reason, "tracker_assist") == 0) return 4;
  if (std::strcmp(reason, "filter_blocked") == 0) return 5;
  if (std::strcmp(reason, "target_lost_hold_expired") == 0) return 6;
  if (std::strcmp(reason, "tracker_primary") == 0) return 7;
  if (std::strcmp(reason, "tracker_lost_hold_expired") == 0) return 8;
  if (std::strcmp(reason, "static_radar_assist") == 0) return 9;
  if (std::strcmp(reason, "static_radar_clear") == 0) return 10;
  if (std::strcmp(reason, "static_radar_unavailable") == 0) return 11;
  if (std::strcmp(reason, "lost_after_exit") == 0) return 12;
  if (std::strcmp(reason, "lost_without_exit") == 0) return 13;
  if (std::strcmp(reason, "lost_after_room_exit") == 0) return 14;
  return 255;
}

uint8_t PresenceReplayLog::motion_reason_code_(const char *value) {
  const char *reason = safe_(value);
  if (std::strcmp(reason, "none") == 0) return 0;
  if (std::strcmp(reason, "moving_target") == 0) return 1;
  if (std::strcmp(reason, "pir") == 0) return 2;
  if (std::strcmp(reason, "motion_hold_expired") == 0) return 3;
  if (std::strcmp(reason, "filter_blocked") == 0) return 4;
  if (std::strcmp(reason, "tracker_motion") == 0) return 5;
  return 255;
}

uint8_t PresenceReplayLog::still_state_code_(const char *value) {
  const char *state = safe_(value);
  if (std::strcmp(state, "idle") == 0) return 0;
  if (std::strcmp(state, "candidate") == 0) return 1;
  if (std::strcmp(state, "confirmed") == 0) return 2;
  if (std::strcmp(state, "moving_decay") == 0) return 3;
  if (std::strcmp(state, "lost") == 0) return 4;
  return 255;
}

uint8_t PresenceReplayLog::still_reason_code_(const char *value) {
  const char *reason = safe_(value);
  if (std::strcmp(reason, "none") == 0) return 0;
  if (std::strcmp(reason, "target_missing") == 0) return 1;
  if (std::strcmp(reason, "moving_target") == 0) return 2;
  if (std::strcmp(reason, "anchor_set") == 0) return 3;
  if (std::strcmp(reason, "same_area_still") == 0) return 4;
  if (std::strcmp(reason, "confidence_expired") == 0) return 5;
  if (std::strcmp(reason, "far_from_anchor") == 0) return 6;
  return 255;
}

uint8_t PresenceReplayLog::range_reason_code_(const char *value) {
  const char *reason = safe_(value);
  if (std::strcmp(reason, "ok") == 0) return 0;
  if (std::strcmp(reason, "invalid_distance") == 0) return 1;
  if (std::strcmp(reason, "out_of_range") == 0) return 2;
  if (std::strcmp(reason, "remote_before_pir") == 0) return 3;
  if (std::strcmp(reason, "remote_pir_validated") == 0) return 4;
  if (std::strcmp(reason, "remote_without_pir") == 0) return 5;
  return 255;
}

uint8_t PresenceReplayLog::tracker_state_code_(const char *value) {
  const char *state = safe_(value);
  if (std::strcmp(state, "idle") == 0) return 0;
  if (std::strcmp(state, "tentative") == 0) return 1;
  if (std::strcmp(state, "confirmed") == 0) return 2;
  if (std::strcmp(state, "coasting") == 0) return 3;
  return 255;
}

uint8_t PresenceReplayLog::tracker_reason_code_(const char *value) {
  const char *reason = safe_(value);
  if (std::strcmp(reason, "none") == 0) return 0;
  if (std::strcmp(reason, "confirmed_by_hits") == 0) return 1;
  if (std::strcmp(reason, "confirmed_by_pir_hint") == 0) return 2;
  if (std::strcmp(reason, "confirmed_reacquired") == 0) return 3;
  if (std::strcmp(reason, "coasting_missed") == 0) return 4;
  if (std::strcmp(reason, "pir") == 0) return 5;
  if (std::strcmp(reason, "idle") == 0) return 6;
  if (std::strcmp(reason, "filter_blocked") == 0) return 7;
  if (std::strcmp(reason, "filter_blocked_missed") == 0) return 8;
  if (std::strcmp(reason, "tentative_new") == 0) return 9;
  if (std::strcmp(reason, "tentative_waiting_hits") == 0) return 10;
  if (std::strcmp(reason, "tentative_missed") == 0) return 11;
  if (std::strcmp(reason, "coasting_after_exit") == 0) return 12;
  if (std::strcmp(reason, "lost_after_exit") == 0) return 13;
  if (std::strcmp(reason, "lost_without_exit") == 0) return 14;
  if (std::strcmp(reason, "coasting_after_room_exit") == 0) return 15;
  if (std::strcmp(reason, "lost_after_room_exit") == 0) return 16;
  return 255;
}

}  // namespace radar_api_server
}  // namespace esphome
