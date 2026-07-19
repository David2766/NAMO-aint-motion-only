#include "presence_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

int clamp_score(int value) {
  if (value < 0)
    return 0;
  if (value > 100)
    return 100;
  return value;
}

}  // namespace

void PresenceTracker::update(const PresenceTrackerInput &input) {
  for (auto &track : this->tracks_)
    track.updated = false;

  if (!input.filter_blocked) {
    int mapping[3] = {-1, -1, -1};
    bool target_used[3] = {false, false, false};
    bool reserved_tracks[3] = {false, false, false};
    int valid_target_count = 0;
    for (const auto &target : input.targets) {
      if (target.valid)
        valid_target_count++;
    }
    this->associate_targets_(input, mapping);

    for (int track_index = 0; track_index < 3; track_index++) {
      const int target_index = mapping[track_index];
      if (target_index < 0)
        continue;
      target_used[target_index] = true;
      reserved_tracks[track_index] = true;
      this->update_track_(this->tracks_[track_index], input.targets[target_index], input);
    }

    const int stable_track_count = this->stable_track_count_();
    for (int target_index = 0; target_index < 3; target_index++) {
      const auto &target = input.targets[target_index];
      if (!target.valid || target_used[target_index])
        continue;
      if (stable_track_count >= valid_target_count)
        continue;

      const int track_index = this->find_reusable_track_(reserved_tracks);
      if (track_index < 0)
        continue;

      reserved_tracks[track_index] = true;
      this->update_track_(this->tracks_[track_index], target, input);
    }
  }

  for (auto &track : this->tracks_) {
    if (!track.updated)
      this->age_track_(track, input);
  }

  this->rebuild_output_(input);
}

std::string PresenceTracker::debug_json() const {
  std::string out;
  out.reserve(520);
  out += R"({"presence":)";
  out += this->output_.presence ? "true" : "false";
  out += R"(,"motion":)";
  out += this->output_.motion ? "true" : "false";
  out += R"(,"state":)";
  append_json_string_(out, this->output_.state);
  out += R"(,"reason":)";
  append_json_string_(out, this->output_.reason);
  out += R"(,"dropReason":)";
  append_json_string_(out, this->output_.drop_reason);
  out += R"(,"dropMs":)";
  out += std::to_string(this->output_.drop_ms);
  out += R"(,"exitTrackCount":)";
  out += std::to_string(this->output_.exit_track_count);
  out += R"(,"trackScore":)";
  out += std::to_string(this->output_.track_score);
  out += R"(,"inputDetectionCount":)";
  out += std::to_string(this->output_.input_detection_count);
  out += R"(,"activeTrackCount":)";
  out += std::to_string(this->output_.active_track_count);
  out += R"(,"tentativeTrackCount":)";
  out += std::to_string(this->output_.tentative_track_count);
  out += R"(,"confirmedTrackCount":)";
  out += std::to_string(this->output_.confirmed_track_count);
  out += R"(,"coastingTrackCount":)";
  out += std::to_string(this->output_.coasting_track_count);
  out += R"(,"movingTrackCount":)";
  out += std::to_string(this->output_.moving_track_count);
  out += R"(,"stillTrackCount":)";
  out += std::to_string(this->output_.still_track_count);
  out += R"(,"tracks":[)";
  bool first = true;
  for (const auto &track : this->tracks_) {
    if (track.state == TrackState::IDLE)
      continue;
    if (!first)
      out += ",";
    first = false;
    out += R"({"state":)";
    append_json_string_(out, state_name_(track.state));
    out += R"(,"trackScore":)";
    out += std::to_string(this->track_score_(track));
    out += R"(,"observedFrames":)";
    out += std::to_string(track.observed_frames);
    out += R"(,"missedFrames":)";
    out += std::to_string(track.missed_frames);
    out += R"(,"recentHits":)";
    out += std::to_string(this->recent_hit_count_(track));
    out += R"(,"reason":)";
    append_json_string_(out, track.reason);
    out += R"(,"stateChangedMs":)";
    out += std::to_string(track.state_changed_ms);
    out += R"(,"moving":)";
    out += track.moving ? "true" : "false";
    out += R"(,"x":)";
    out += std::to_string(static_cast<int>(std::lround(track.x_mm)));
    out += R"(,"y":)";
    out += std::to_string(static_cast<int>(std::lround(track.y_mm)));
    out += R"(,"vx":)";
    out += std::to_string(static_cast<int>(std::lround(track.vx_mm_s)));
    out += R"(,"vy":)";
    out += std::to_string(static_cast<int>(std::lround(track.vy_mm_s)));
    out += R"(,"speedCmS":)";
    out += std::to_string(static_cast<int>(std::lround(track.speed_cm_s)));
    out += R"(,"distance":)";
    out += std::to_string(static_cast<int>(std::lround(track.distance_mm)));
    out += R"(,"radialVelocityCmS":)";
    out += std::to_string(static_cast<int>(std::lround(track.radial_velocity_cm_s)));
    out += R"(,"direction":)";
    append_json_string_(out, direction_name_(track.direction));
    out += R"(,"exitMask":)";
    out += std::to_string(track.exit_zone_mask);
    out += R"(,"exitAgeMs":)";
    const int exit_age_ms = this->exit_age_ms_(track, track.last_update_ms);
    if (exit_age_ms < 0)
      out += "null";
    else
      out += std::to_string(exit_age_ms);
    out += R"(,"exitRecent":)";
    out += this->has_recent_exit_evidence_(track, track.last_update_ms) ? "true" : "false";
    out += R"(,"roomState":)";
    append_json_string_(out, this->room_state_name_(track));
    out += R"(,"roomDistance":)";
    if (track.room_distance_valid)
      out += std::to_string(static_cast<int>(std::lround(track.last_room_signed_distance_mm)));
    else
      out += "null";
    out += R"(,"roomExitEvidence":)";
    out += this->has_room_exit_evidence_(track) ? "true" : "false";
    out += R"(,"roomExitReason":)";
    append_json_string_(out, track.room_exit_reason);
    out += "}";
  }
  out += "]}";
  return out;
}

void PresenceTracker::associate_targets_(const PresenceTrackerInput &input, int (&mapping)[3]) const {
  int track_indices[3] = {-1, -1, -1};
  int target_indices[3] = {-1, -1, -1};
  int track_count = 0;
  int target_count = 0;

  for (int i = 0; i < 3; i++) {
    mapping[i] = -1;
    if (this->tracks_[i].state != TrackState::IDLE)
      track_indices[track_count++] = i;
    if (input.targets[i].valid)
      target_indices[target_count++] = i;
  }

  if (track_count == 0 || target_count == 0)
    return;

  int best_mapping[3] = {-1, -1, -1};
  int best_match_count = -1;
  float best_cost = 1.0e12f;

  for (int c0 = -1; c0 < target_count; c0++) {
    for (int c1 = -1; c1 < target_count; c1++) {
      for (int c2 = -1; c2 < target_count; c2++) {
        const int choices[3] = {c0, c1, c2};
        bool used_targets[3] = {false, false, false};
        int candidate_mapping[3] = {-1, -1, -1};
        int match_count = 0;
        float cost = 0.0f;
        bool valid = true;

        for (int pos = 0; pos < track_count; pos++) {
          const int choice = choices[pos];
          if (choice < 0)
            continue;
          if (used_targets[choice]) {
            valid = false;
            break;
          }

          const int track_index = track_indices[pos];
          const int target_index = target_indices[choice];
          const auto &track = this->tracks_[track_index];
          const auto &target = input.targets[target_index];
          const float distance = distance_between_(target.x_mm, target.y_mm, this->predicted_x_(track, input.now_ms),
                                                   this->predicted_y_(track, input.now_ms));
          if (distance > this->association_radius_for_(track)) {
            valid = false;
            break;
          }

          used_targets[choice] = true;
          candidate_mapping[track_index] = target_index;
          match_count++;
          cost += this->association_cost_(track, distance);
        }

        if (!valid)
          continue;

        if (match_count > best_match_count || (match_count == best_match_count && cost < best_cost)) {
          best_match_count = match_count;
          best_cost = cost;
          for (int i = 0; i < 3; i++)
            best_mapping[i] = candidate_mapping[i];
        }
      }
    }
  }

  if (best_match_count <= 0)
    return;

  for (int i = 0; i < 3; i++)
    mapping[i] = best_mapping[i];
}

int PresenceTracker::find_reusable_track_(const bool (&reserved_tracks)[3]) const {
  for (int i = 0; i < 3; i++) {
    if (!reserved_tracks[i] && this->tracks_[i].state == TrackState::IDLE)
      return i;
  }

  int weakest_index = -1;
  int weakest_score = 101;
  for (int i = 0; i < 3; i++) {
    if (reserved_tracks[i])
      continue;
    const auto &track = this->tracks_[i];
    if (track.state != TrackState::TENTATIVE)
      continue;
    const int score = this->track_score_(track);
    if (score < weakest_score) {
      weakest_score = score;
      weakest_index = i;
    }
  }
  return weakest_index;
}

void PresenceTracker::update_track_(Track &track, const PresenceTrackerTargetInput &target,
                                    const PresenceTrackerInput &input) {
  const bool was_idle = track.state == TrackState::IDLE;

  if (was_idle) {
    track.state = TrackState::TENTATIVE;
    track.first_seen_ms = input.now_ms;
    track.state_changed_ms = input.now_ms;
    track.reason = "tentative_new";
    this->reset_filter_(track, target.x_mm, target.y_mm, input.now_ms);
  }

  const float previous_filtered_x = track.x_mm;
  const float previous_filtered_y = track.y_mm;

  if (!track.filter_initialized) {
    this->reset_filter_(track, target.x_mm, target.y_mm, input.now_ms);
  } else if (!was_idle) {
    const float predicted_x = this->predicted_x_(track, input.now_ms);
    const float predicted_y = this->predicted_y_(track, input.now_ms);
    const float jump_distance = distance_between_(target.x_mm, target.y_mm, predicted_x, predicted_y);
    if (jump_distance > this->policy_.teleport_reset_distance_mm) {
      this->reset_filter_(track, target.x_mm, target.y_mm, input.now_ms);
    } else {
      this->update_filter_(track, target.x_mm, target.y_mm, input.now_ms);
    }
  }

  this->refresh_track_kinematics_(track, input);
  const float movement_mm = was_idle ? 0.0f : distance_between_(track.x_mm, track.y_mm, previous_filtered_x, previous_filtered_y);
  const float moving_enter_speed = input.stationary_speed_cm_s * this->policy_.moving_enter_speed_multiplier;
  const float moving_exit_speed = input.stationary_speed_cm_s * this->policy_.moving_exit_speed_multiplier;
  const bool speed_moving = track.moving ? track.speed_cm_s > moving_exit_speed : track.speed_cm_s > moving_enter_speed;
  const bool position_moving = movement_mm > this->policy_.stationary_radius_mm;

  track.moving = speed_moving || position_moving;
  track.last_seen_ms = input.now_ms;
  track.last_update_ms = input.now_ms;
  track.missed_frames = 0;
  this->update_exit_evidence_(track, target, input.now_ms);
  this->update_room_evidence_(track, target, input);
  this->last_drop_reason_ = "none";
  this->last_drop_ms_ = 0;
  if (track.observed_frames < 65535)
    track.observed_frames++;
  track.updated = true;
  this->record_track_observation_(track, true);

  if (track.state == TrackState::COASTING) {
    track.state = TrackState::CONFIRMED;
    track.state_changed_ms = input.now_ms;
    track.reason = "confirmed_reacquired";
  } else if (track.state == TrackState::TENTATIVE && this->should_confirm_(track, input)) {
    const int recent_hits = this->recent_hit_count_(track);
    const bool pir_fast_confirm = input.pir_motion && recent_hits >= this->policy_.pir_confirm_hits_required &&
                                  recent_hits < this->policy_.confirm_hits_required;
    track.state = TrackState::CONFIRMED;
    track.state_changed_ms = input.now_ms;
    track.reason = pir_fast_confirm ? "confirmed_by_pir_hint" : "confirmed_by_hits";
  } else if (track.state == TrackState::TENTATIVE) {
    track.reason = "tentative_waiting_hits";
  }
}

void PresenceTracker::age_track_(Track &track, const PresenceTrackerInput &input) {
  if (track.state == TrackState::IDLE)
    return;

  this->predict_filter_(track, input.now_ms);
  this->refresh_track_kinematics_(track, input);
  if (track.missed_frames < 65535)
    track.missed_frames++;
  this->record_track_observation_(track, false);

  if (track.state == TrackState::TENTATIVE) {
    track.reason = input.filter_blocked ? "filter_blocked_missed" : "tentative_missed";
    if (track.missed_frames > this->policy_.tentative_delete_missed_frames)
      track = Track{};
    return;
  }

  if (track.state == TrackState::CONFIRMED) {
    track.state = TrackState::COASTING;
    track.state_changed_ms = input.now_ms;
    const bool exit_recent = this->has_recent_exit_evidence_(track, input.now_ms);
    const bool room_exit = this->has_room_exit_evidence_(track);
    track.reason = input.filter_blocked ? "filter_blocked_missed" :
                   exit_recent ? "coasting_after_exit" :
                   room_exit ? "coasting_after_room_exit" :
                   "coasting_missed";
  }

  if (track.state == TrackState::COASTING &&
      track.missed_frames > this->coasting_delete_missed_frames_for_(track, input.now_ms)) {
    const bool exit_recent = this->has_recent_exit_evidence_(track, input.now_ms);
    const bool room_exit = this->has_room_exit_evidence_(track);
    this->last_drop_reason_ = exit_recent ? "lost_after_exit" :
                              room_exit ? "lost_after_room_exit" :
                              "lost_without_exit";
    this->last_drop_ms_ = input.now_ms;
    track = Track{};
  }
}

void PresenceTracker::rebuild_output_(const PresenceTrackerInput &input) {
  PresenceTrackerOutput output;
  output.reason = input.filter_blocked ? "filter_blocked" : "none";
  output.drop_reason = this->last_drop_reason_;
  output.drop_ms = this->last_drop_ms_;
  output.input_detection_count = 0;
  for (const auto &target : input.targets) {
    if (target.valid)
      output.input_detection_count++;
  }

  for (const auto &track : this->tracks_) {
    if (track.state == TrackState::IDLE)
      continue;

    output.active_track_count++;
    const bool exit_recent = this->has_recent_exit_evidence_(track, input.now_ms);
    const int exit_age_ms = this->exit_age_ms_(track, input.now_ms);
    if (exit_recent)
      output.exit_track_count++;
    const int score = this->track_score_(track);
    if (score > output.track_score) {
      output.track_score = score;
      output.state = state_name_(track.state);
    }

    if (track.state == TrackState::TENTATIVE) {
      output.tentative_track_count++;
      continue;
    }

    if (track.state == TrackState::CONFIRMED) {
      output.presence = true;
      output.confirmed_track_count++;
      output.reason = track.reason;
      const int slot = output.confirmed_track_count + output.coasting_track_count - 1;
      if (slot >= 0 && slot < static_cast<int>(output.tracks.size())) {
        output.tracks[slot] = {true, track.moving, false, track.x_mm, track.y_mm, track.vx_mm_s, track.vy_mm_s,
                               track.speed_cm_s, track.distance_mm, track.radial_velocity_cm_s, score,
                               state_name_(track.state), track.reason, direction_name_(track.direction),
                               track.exit_zone_mask, exit_age_ms, exit_recent, this->room_state_name_(track)};
      }
      if (track.moving) {
        output.motion = true;
        output.moving_track_count++;
      } else {
        output.still_track_count++;
      }
      continue;
    }

    if (track.state == TrackState::COASTING) {
      output.presence = true;
      output.coasting_track_count++;
      const int slot = output.confirmed_track_count + output.coasting_track_count - 1;
      if (slot >= 0 && slot < static_cast<int>(output.tracks.size())) {
        output.tracks[slot] = {true, track.moving, true, track.x_mm, track.y_mm, track.vx_mm_s, track.vy_mm_s,
                               track.speed_cm_s, track.distance_mm, track.radial_velocity_cm_s, score,
                               state_name_(track.state), track.reason, direction_name_(track.direction),
                               track.exit_zone_mask, exit_age_ms, exit_recent, this->room_state_name_(track)};
      }
      output.reason = track.reason;
    }
  }

  this->output_ = output;
}

void PresenceTracker::record_track_observation_(Track &track, bool observed) {
  uint8_t window = this->policy_.confirm_window_frames;
  if (window == 0)
    window = 1;
  if (window > 8)
    window = 8;

  const uint8_t mask = static_cast<uint8_t>((1U << window) - 1U);
  track.hit_mask = static_cast<uint8_t>(((track.hit_mask << 1U) | (observed ? 1U : 0U)) & mask);
  if (track.history_frames < window)
    track.history_frames++;
}

void PresenceTracker::update_exit_evidence_(Track &track, const PresenceTrackerTargetInput &target, uint32_t now_ms) {
  if (target.exit_zone_mask == 0)
    return;
  track.exit_zone_mask = target.exit_zone_mask;
  track.last_exit_seen_ms = now_ms;
}

void PresenceTracker::update_room_evidence_(Track &track, const PresenceTrackerTargetInput &target,
                                            const PresenceTrackerInput &input) {
  if (!input.room_context_configured)
    return;

  const bool had_distance = track.room_distance_valid;
  const float previous_distance = track.last_room_signed_distance_mm;
  const float current_distance = target.room_signed_distance_mm;
  const float margin = this->room_boundary_margin_mm_(track);
  const bool confidently_inside = current_distance > margin;
  const bool confidently_outside = current_distance < -margin;
  const bool crossed_outward = had_distance && previous_distance > margin && current_distance < -margin;
  const bool moved_outward = had_distance && (previous_distance - current_distance) > margin;

  track.room_context_seen = true;
  track.last_room_inside = target.room_inside;
  track.last_room_outside = target.room_outside;
  track.last_room_signed_distance_mm = current_distance;
  track.room_distance_valid = true;

  if (confidently_inside) {
    track.room_exit_candidate = false;
    track.room_exit_crossed = false;
    track.room_exit_outward = false;
    track.room_exit_reason = "inside_room";
    return;
  }

  if (crossed_outward && moved_outward) {
    track.room_exit_candidate = true;
    track.room_exit_crossed = true;
    track.room_exit_outward = true;
    track.room_exit_reason = "room_boundary_crossed";
    return;
  }

  if (confidently_outside && track.room_exit_candidate) {
    track.room_exit_reason = "room_exit_confirmed";
    return;
  }

  if (!confidently_outside)
    track.room_exit_reason = "room_boundary_ambiguous";
}

bool PresenceTracker::has_recent_exit_evidence_(const Track &track, uint32_t now_ms) const {
  if (track.last_exit_seen_ms == 0 || track.exit_zone_mask == 0)
    return false;
  return now_ms >= track.last_exit_seen_ms && (now_ms - track.last_exit_seen_ms) <= this->policy_.exit_evidence_max_age_ms;
}

int PresenceTracker::exit_age_ms_(const Track &track, uint32_t now_ms) const {
  if (track.last_exit_seen_ms == 0 || track.exit_zone_mask == 0 || now_ms < track.last_exit_seen_ms)
    return -1;
  return static_cast<int>(now_ms - track.last_exit_seen_ms);
}

uint8_t PresenceTracker::coasting_delete_missed_frames_for_(const Track &track, uint32_t now_ms) const {
  if (this->has_recent_exit_evidence_(track, now_ms))
    return this->policy_.exit_coasting_delete_missed_frames;
  if (this->has_room_exit_evidence_(track))
    return this->policy_.coasting_delete_missed_frames;
  if (track.exit_zone_mask == 0)
    return this->policy_.non_exit_coasting_delete_missed_frames;
  return this->policy_.coasting_delete_missed_frames;
}

const char *PresenceTracker::room_state_name_(const Track &track) const {
  if (!track.room_context_seen)
    return "unknown";
  if (track.last_room_inside)
    return "inside";
  if (track.last_room_outside)
    return "outside";
  return "unknown";
}

bool PresenceTracker::has_room_exit_evidence_(const Track &track) const {
  return track.room_exit_candidate && track.room_exit_crossed && track.room_exit_outward && track.last_room_outside;
}

float PresenceTracker::room_boundary_margin_mm_(const Track &track) const {
  const float position_variance = std::max(0.0f, (track.covariance[0][0] + track.covariance[1][1]) * 0.5f);
  return std::sqrt(position_variance + this->policy_.measurement_noise_mm2);
}

bool PresenceTracker::should_confirm_(const Track &track, const PresenceTrackerInput &input) const {
  const int required_hits = input.pir_motion ? this->policy_.pir_confirm_hits_required : this->policy_.confirm_hits_required;
  return track.history_frames >= required_hits && this->recent_hit_count_(track) >= required_hits;
}

int PresenceTracker::recent_hit_count_(const Track &track) const {
  int count = 0;
  uint8_t mask = track.hit_mask;
  for (uint8_t i = 0; i < track.history_frames; i++) {
    if ((mask & 1U) != 0)
      count++;
    mask = static_cast<uint8_t>(mask >> 1U);
  }
  return count;
}

int PresenceTracker::track_score_(const Track &track) const {
  int score = this->recent_hit_count_(track) * 18;
  score += std::min<int>(track.observed_frames, 4) * 6;
  score -= std::min<int>(track.missed_frames, 8) * 8;

  if (track.state == TrackState::CONFIRMED)
    score += 20;
  else if (track.state == TrackState::COASTING)
    score += 10;

  return clamp_score(score);
}

int PresenceTracker::stable_track_count_() const {
  int count = 0;
  for (const auto &track : this->tracks_) {
    if (is_stable_track_(track))
      count++;
  }
  return count;
}

float PresenceTracker::association_radius_for_(const Track &track) const {
  return is_stable_track_(track) ? this->policy_.stable_track_reassociation_radius_mm
                                 : this->policy_.association_radius_mm;
}

float PresenceTracker::association_cost_(const Track &track, float distance) const {
  float cost = distance;
  if (track.state == TrackState::TENTATIVE)
    cost += this->policy_.stable_track_reassociation_radius_mm;
  else if (track.state == TrackState::COASTING)
    cost += static_cast<float>(std::min<int>(track.missed_frames, 8)) * 40.0f;
  return cost;
}

float PresenceTracker::predicted_x_(const Track &track, uint32_t now_ms) const {
  if (!track.filter_initialized || track.last_update_ms == 0 || now_ms <= track.last_update_ms)
    return track.x_mm;

  uint32_t dt_ms = now_ms - track.last_update_ms;
  if (this->policy_.max_filter_dt_ms > 0)
    dt_ms = std::min<uint32_t>(dt_ms, this->policy_.max_filter_dt_ms);
  return track.x_mm + track.vx_mm_s * (static_cast<float>(dt_ms) / 1000.0f);
}

float PresenceTracker::predicted_y_(const Track &track, uint32_t now_ms) const {
  if (!track.filter_initialized || track.last_update_ms == 0 || now_ms <= track.last_update_ms)
    return track.y_mm;

  uint32_t dt_ms = now_ms - track.last_update_ms;
  if (this->policy_.max_filter_dt_ms > 0)
    dt_ms = std::min<uint32_t>(dt_ms, this->policy_.max_filter_dt_ms);
  return track.y_mm + track.vy_mm_s * (static_cast<float>(dt_ms) / 1000.0f);
}

void PresenceTracker::reset_filter_(Track &track, float x_mm, float y_mm, uint32_t now_ms) const {
  track.filter_initialized = true;
  track.x_mm = x_mm;
  track.y_mm = y_mm;
  track.vx_mm_s = 0.0f;
  track.vy_mm_s = 0.0f;
  track.speed_cm_s = 0.0f;
  track.last_update_ms = now_ms;

  std::memset(track.covariance, 0, sizeof(track.covariance));
  track.covariance[0][0] = this->policy_.initial_position_covariance;
  track.covariance[1][1] = this->policy_.initial_position_covariance;
  track.covariance[2][2] = this->policy_.initial_velocity_covariance;
  track.covariance[3][3] = this->policy_.initial_velocity_covariance;
}

void PresenceTracker::predict_filter_(Track &track, uint32_t now_ms) const {
  if (!track.filter_initialized) {
    track.last_update_ms = now_ms;
    return;
  }
  if (track.last_update_ms == 0) {
    track.last_update_ms = now_ms;
    return;
  }
  if (now_ms <= track.last_update_ms)
    return;

  uint32_t dt_ms = now_ms - track.last_update_ms;
  if (this->policy_.max_filter_dt_ms > 0)
    dt_ms = std::min<uint32_t>(dt_ms, this->policy_.max_filter_dt_ms);
  const float dt = static_cast<float>(dt_ms) / 1000.0f;
  if (dt <= 0.0f)
    return;

  track.x_mm += track.vx_mm_s * dt;
  track.y_mm += track.vy_mm_s * dt;

  float predicted_covariance[4][4]{};
  float fp[4][4]{};
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      fp[row][col] = track.covariance[row][col];
      if (row < 2)
        fp[row][col] += dt * track.covariance[row + 2][col];
    }
  }

  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      predicted_covariance[row][col] = fp[row][col];
      if (col < 2)
        predicted_covariance[row][col] += dt * fp[row][col + 2];
    }
  }

  const float dt2 = dt * dt;
  const float dt3_half = dt2 * dt * 0.5f;
  const float dt4_quarter = dt2 * dt2 * 0.25f;
  const float q = this->policy_.process_noise_accel_mm2_s4;
  predicted_covariance[0][0] += q * dt4_quarter;
  predicted_covariance[1][1] += q * dt4_quarter;
  predicted_covariance[2][2] += q * dt2;
  predicted_covariance[3][3] += q * dt2;
  predicted_covariance[0][2] += q * dt3_half;
  predicted_covariance[2][0] += q * dt3_half;
  predicted_covariance[1][3] += q * dt3_half;
  predicted_covariance[3][1] += q * dt3_half;

  std::memcpy(track.covariance, predicted_covariance, sizeof(track.covariance));
  track.last_update_ms = now_ms;
}

void PresenceTracker::update_filter_(Track &track, float x_mm, float y_mm, uint32_t now_ms) const {
  if (!track.filter_initialized) {
    this->reset_filter_(track, x_mm, y_mm, now_ms);
    return;
  }

  this->predict_filter_(track, now_ms);

  const float innovation_x = x_mm - track.x_mm;
  const float innovation_y = y_mm - track.y_mm;
  const float measurement_noise = this->policy_.measurement_noise_mm2;
  const float s00 = track.covariance[0][0] + measurement_noise;
  const float s01 = track.covariance[0][1];
  const float s10 = track.covariance[1][0];
  const float s11 = track.covariance[1][1] + measurement_noise;
  const float det = s00 * s11 - s01 * s10;
  if (std::fabs(det) < 1.0e-6f) {
    this->reset_filter_(track, x_mm, y_mm, now_ms);
    return;
  }

  const float inv_det = 1.0f / det;
  const float inv_s00 = s11 * inv_det;
  const float inv_s01 = -s01 * inv_det;
  const float inv_s10 = -s10 * inv_det;
  const float inv_s11 = s00 * inv_det;

  float kalman_gain[4][2]{};
  for (int row = 0; row < 4; row++) {
    kalman_gain[row][0] = track.covariance[row][0] * inv_s00 + track.covariance[row][1] * inv_s10;
    kalman_gain[row][1] = track.covariance[row][0] * inv_s01 + track.covariance[row][1] * inv_s11;
  }

  track.x_mm += kalman_gain[0][0] * innovation_x + kalman_gain[0][1] * innovation_y;
  track.y_mm += kalman_gain[1][0] * innovation_x + kalman_gain[1][1] * innovation_y;
  track.vx_mm_s += kalman_gain[2][0] * innovation_x + kalman_gain[2][1] * innovation_y;
  track.vy_mm_s += kalman_gain[3][0] * innovation_x + kalman_gain[3][1] * innovation_y;

  float updated_covariance[4][4]{};
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      updated_covariance[row][col] = track.covariance[row][col] - kalman_gain[row][0] * track.covariance[0][col] -
                                     kalman_gain[row][1] * track.covariance[1][col];
    }
  }
  std::memcpy(track.covariance, updated_covariance, sizeof(track.covariance));
}

void PresenceTracker::refresh_track_kinematics_(Track &track, const PresenceTrackerInput &input) const {
  track.speed_cm_s = std::sqrt(track.vx_mm_s * track.vx_mm_s + track.vy_mm_s * track.vy_mm_s) / 10.0f;
  track.distance_mm = std::sqrt(track.x_mm * track.x_mm + track.y_mm * track.y_mm);
  if (track.distance_mm >= this->policy_.minimum_direction_distance_mm) {
    track.radial_velocity_cm_s =
        ((track.x_mm * track.vx_mm_s + track.y_mm * track.vy_mm_s) / track.distance_mm) / 10.0f;
  } else {
    track.radial_velocity_cm_s = 0.0f;
  }
  this->update_direction_(track, input);
}

void PresenceTracker::update_direction_(Track &track, const PresenceTrackerInput &input) const {
  const float enter_threshold = input.stationary_speed_cm_s * this->policy_.direction_enter_speed_multiplier;
  const float exit_threshold = input.stationary_speed_cm_s * this->policy_.direction_exit_speed_multiplier;
  const float radial_velocity = track.radial_velocity_cm_s;

  switch (track.direction) {
    case TrackDirection::APPROACHING:
      if (radial_velocity > enter_threshold) {
        track.direction = TrackDirection::MOVING_AWAY;
      } else if (radial_velocity >= -exit_threshold) {
        track.direction = TrackDirection::STATIONARY;
      }
      break;
    case TrackDirection::MOVING_AWAY:
      if (radial_velocity < -enter_threshold) {
        track.direction = TrackDirection::APPROACHING;
      } else if (radial_velocity <= exit_threshold) {
        track.direction = TrackDirection::STATIONARY;
      }
      break;
    case TrackDirection::STATIONARY:
    default:
      if (radial_velocity < -enter_threshold) {
        track.direction = TrackDirection::APPROACHING;
      } else if (radial_velocity > enter_threshold) {
        track.direction = TrackDirection::MOVING_AWAY;
      } else {
        track.direction = TrackDirection::STATIONARY;
      }
      break;
  }
}

bool PresenceTracker::is_stable_track_(const Track &track) {
  return track.state == TrackState::CONFIRMED || track.state == TrackState::COASTING;
}

float PresenceTracker::distance_between_(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

const char *PresenceTracker::state_name_(TrackState state) {
  switch (state) {
    case TrackState::TENTATIVE:
      return "tentative";
    case TrackState::CONFIRMED:
      return "confirmed";
    case TrackState::COASTING:
      return "coasting";
    case TrackState::IDLE:
    default:
      return "idle";
  }
}

const char *PresenceTracker::direction_name_(TrackDirection direction) {
  switch (direction) {
    case TrackDirection::APPROACHING:
      return "approaching";
    case TrackDirection::MOVING_AWAY:
      return "moving_away";
    case TrackDirection::STATIONARY:
    default:
      return "stationary";
  }
}

void PresenceTracker::append_json_string_(std::string &out, const char *value) {
  out.push_back('"');
  const char *safe = value != nullptr && value[0] != '\0' ? value : "none";
  for (const char *p = safe; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(ch));
    } else if (ch < 0x20) {
      char buf[7];
      std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
      out += buf;
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  out.push_back('"');
}

}  // namespace radar_api_server
}  // namespace esphome
