#include "presence_fusion.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace radar_api_server {

void PresenceFusion::update(const PresenceFusionInput &input, const PresenceTrackerOutput &tracker) {
  PresenceFusionOutput output;

  this->update_static_distance_(input.now_ms, input.static_radar);
  StaticRadarEvidence stable_static_radar = input.static_radar;
  stable_static_radar.detection_distance_mm = this->stable_static_distance_mm_;

  const bool stable_presence_ended = this->previous_stable_presence_ && !input.stable_presence;
  if (input.stable_presence) {
    this->static_assist_reentry_pending_ = false;
    this->static_assist_reentry_started_ms_ = 0;
  } else {
    if (stable_presence_ended && this->static_assist_armed_ && !this->static_assist_exit_veto_ &&
        !input.filter_blocked) {
      this->static_assist_reentry_pending_ = true;
      this->static_assist_reentry_started_ms_ = input.now_ms;
    }

    const bool reentry_expired = this->static_assist_reentry_pending_ &&
        (input.now_ms - this->static_assist_reentry_started_ms_) >= STATIC_ASSIST_REENTRY_MS;
    if (!this->static_assist_reentry_pending_ || reentry_expired || input.filter_blocked)
      this->reset_session_();
  }

  const bool pir_rising = input.pir_motion && !this->previous_pir_motion_;
  const bool tracker_evidence = !input.filter_blocked && tracker.presence;
  const bool base_presence = input.pir_motion || tracker_evidence;
  const bool static_present = input.static_radar.available && input.static_radar.presence;
  const bool new_drop = tracker.drop_ms != 0 && tracker.drop_ms != this->last_processed_drop_ms_;

  this->update_last_spatial_target_(tracker);

  if (new_drop) {
    this->last_processed_drop_ms_ = tracker.drop_ms;
    this->static_assist_exit_veto_ = is_exit_drop_reason_(tracker.drop_reason);
    this->static_assist_exit_reason_ = this->static_assist_exit_veto_ ? tracker.drop_reason : "none";
    if (this->static_assist_exit_veto_)
      this->last_spatial_target_valid_ = false;
  }

  if (tracker.confirmed_track_count > 0 || (pir_rising && input.stable_presence)) {
    this->static_assist_exit_veto_ = false;
    this->static_assist_exit_reason_ = "none";
  }

  const bool can_arm = !input.filter_blocked && !this->static_assist_exit_veto_ && base_presence && static_present;
  if (!this->static_assist_armed_ && can_arm) {
    if (!this->static_assist_arm_pending_) {
      this->static_assist_arm_pending_ = true;
      this->static_assist_arm_started_ms_ = input.now_ms;
    } else if ((input.now_ms - this->static_assist_arm_started_ms_) >= STATIC_ASSIST_ARM_MS) {
      this->static_assist_armed_ = true;
      this->static_assist_arm_pending_ = false;
    }
  } else if (!this->static_assist_armed_) {
    this->static_assist_arm_pending_ = false;
    this->static_assist_arm_started_ms_ = 0;
  }

  const bool static_assist_active = !input.filter_blocked && !this->static_assist_exit_veto_ &&
                                    this->static_assist_armed_ && static_present && !base_presence;

  output.presence = base_presence || static_assist_active;
  output.motion = !input.filter_blocked && (input.pir_motion || tracker.motion);
  output.static_radar = stable_static_radar;
  output.static_radar_reason = !input.static_radar.available ? "unavailable" :
                               input.static_radar.moving ? "moving" :
                               input.static_radar.still ? "still" :
                               input.static_radar.presence ? "target" :
                               "clear";

  output.presence_reason = input.pir_motion ? "pir" :
                           input.filter_blocked ? "filter_blocked" :
                           tracker_evidence ? "tracker_primary" :
                           static_assist_active ? "static_radar_assist" :
                           "none";
  output.motion_reason = input.filter_blocked ? "filter_blocked" :
                         input.pir_motion ? "pir" :
                         tracker.motion ? "tracker_motion" :
                         "none";
  output.presence_off_reason = input.filter_blocked ? "filter_blocked" :
                               this->static_assist_exit_veto_ ? this->static_assist_exit_reason_ :
                               this->static_assist_armed_ && !base_presence && !input.static_radar.available ?
                                 "static_radar_unavailable" :
                               this->static_assist_armed_ && !base_presence && !static_present ?
                                 "static_radar_clear" :
                               has_drop_reason_(tracker.drop_reason) ? tracker.drop_reason :
                               "tracker_lost_hold_expired";
  output.pir_evidence = input.pir_motion;
  output.tracker_evidence = tracker_evidence;
  output.static_assist_armed = this->static_assist_armed_;
  output.static_assist_active = static_assist_active;
  output.static_assist_arm_pending = this->static_assist_arm_pending_;
  output.static_assist_exit_veto = this->static_assist_exit_veto_;
  output.static_assist_arm_elapsed_ms = this->static_assist_arm_pending_ ?
      input.now_ms - this->static_assist_arm_started_ms_ : 0;

  if (static_assist_active && this->last_spatial_target_valid_) {
    const float static_distance_mm = static_radar_distance_mm_(stable_static_radar);
    const bool distance_comparable = std::isfinite(static_distance_mm) && static_distance_mm > 0.0f;
    const float distance_delta_mm = distance_comparable ?
        std::fabs(static_distance_mm - this->last_spatial_target_distance_mm_) : 0.0f;
    output.static_assist_held_target = {
        true,
        distance_comparable,
        distance_comparable && distance_delta_mm <= STATIC_ASSIST_TARGET_DISTANCE_TOLERANCE_MM,
        this->last_spatial_target_x_mm_,
        this->last_spatial_target_y_mm_,
        this->last_spatial_target_distance_mm_,
        static_distance_mm,
        distance_delta_mm,
    };
  }

  this->output_ = output;
  this->previous_pir_motion_ = input.pir_motion;
  this->previous_stable_presence_ = input.stable_presence;
}

bool PresenceFusion::has_drop_reason_(const char *value) {
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "none") != 0;
}

bool PresenceFusion::is_exit_drop_reason_(const char *value) {
  return value != nullptr &&
         (std::strcmp(value, "lost_after_exit") == 0 || std::strcmp(value, "lost_after_room_exit") == 0);
}

float PresenceFusion::static_radar_distance_mm_(const StaticRadarEvidence &evidence) {
  if (evidence.presence && std::isfinite(evidence.detection_distance_mm) && evidence.detection_distance_mm > 0.0f)
    return evidence.detection_distance_mm;
  return 0.0f;
}

void PresenceFusion::update_static_distance_(uint32_t now_ms, const StaticRadarEvidence &evidence) {
  if (!evidence.available || !evidence.presence) {
    this->reset_static_distance_();
    return;
  }
  if (!std::isfinite(evidence.detection_distance_mm) || evidence.detection_distance_mm <= 0.0f)
    return;
  if (this->static_distance_sample_count_ > 0 &&
      (now_ms - this->static_distance_last_sample_ms_) < STATIC_DISTANCE_SAMPLE_MS)
    return;

  this->static_distance_last_sample_ms_ = now_ms;
  this->static_distance_samples_[this->static_distance_next_sample_] = evidence.detection_distance_mm;
  this->static_distance_next_sample_ = (this->static_distance_next_sample_ + 1) % 3;
  if (this->static_distance_sample_count_ < 3)
    this->static_distance_sample_count_++;

  if (this->static_distance_sample_count_ == 1) {
    this->stable_static_distance_mm_ = evidence.detection_distance_mm;
    return;
  }
  if (this->static_distance_sample_count_ < 3)
    return;

  const float a = this->static_distance_samples_[0];
  const float b = this->static_distance_samples_[1];
  const float c = this->static_distance_samples_[2];
  this->stable_static_distance_mm_ = std::max(std::min(a, b), std::min(std::max(a, b), c));
}

void PresenceFusion::reset_static_distance_() {
  this->static_distance_samples_[0] = 0.0f;
  this->static_distance_samples_[1] = 0.0f;
  this->static_distance_samples_[2] = 0.0f;
  this->stable_static_distance_mm_ = 0.0f;
  this->static_distance_last_sample_ms_ = 0;
  this->static_distance_sample_count_ = 0;
  this->static_distance_next_sample_ = 0;
}

void PresenceFusion::update_last_spatial_target_(const PresenceTrackerOutput &tracker) {
  const PresenceTrackerTrackOutput *only_track = nullptr;
  int valid_track_count = 0;
  for (const auto &track : tracker.tracks) {
    if (!track.valid)
      continue;
    only_track = &track;
    valid_track_count++;
  }

  if (valid_track_count > 1) {
    this->last_spatial_target_valid_ = false;
    return;
  }
  if (valid_track_count != 1 || only_track == nullptr)
    return;
  if (!std::isfinite(only_track->x_mm) || !std::isfinite(only_track->y_mm))
    return;

  const float distance_mm = std::hypot(only_track->x_mm, only_track->y_mm);
  if (!std::isfinite(distance_mm) || distance_mm <= 0.0f)
    return;

  this->last_spatial_target_valid_ = true;
  this->last_spatial_target_x_mm_ = only_track->x_mm;
  this->last_spatial_target_y_mm_ = only_track->y_mm;
  this->last_spatial_target_distance_mm_ = distance_mm;
}

void PresenceFusion::reset_session_() {
  this->static_assist_armed_ = false;
  this->static_assist_arm_pending_ = false;
  this->static_assist_exit_veto_ = false;
  this->static_assist_reentry_pending_ = false;
  this->static_assist_exit_reason_ = "none";
  this->static_assist_arm_started_ms_ = 0;
  this->static_assist_reentry_started_ms_ = 0;
  this->last_spatial_target_valid_ = false;
  this->last_spatial_target_x_mm_ = 0.0f;
  this->last_spatial_target_y_mm_ = 0.0f;
  this->last_spatial_target_distance_mm_ = 0.0f;
}

}  // namespace radar_api_server
}  // namespace esphome
