#include "local_observation.h"

#include "presence_fusion.h"
#include "presence_tracker.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

void LocalObservationAdapter::update(uint32_t now_ms, bool pir_motion, bool filter_blocked,
                                     const PresenceTrackerOutput &tracker,
                                     const PresenceFusionOutput &fusion) {
  LocalObservation observation;
  observation.sequence = this->allocate_sequence_();
  observation.observed_at_ms = now_ms;

  for (const auto &source : tracker.tracks) {
    if (!source.valid || source.local_track_id == 0 ||
        observation.track_count >= observation.tracks.size())
      continue;

    auto &target = observation.tracks[observation.track_count++];
    target.valid = true;
    target.local_track_id = source.local_track_id;
    target.moving = source.moving;
    target.coasting = source.coasting;
    target.sample_age_ms = source.sample_age_ms;
    target.x_mm = source.x_mm;
    target.y_mm = source.y_mm;
    target.vx_mm_s = source.vx_mm_s;
    target.vy_mm_s = source.vy_mm_s;
    target.position_variance_x_mm2 = source.position_variance_x_mm2;
    target.position_variance_y_mm2 = source.position_variance_y_mm2;
    target.distance_mm = source.distance_mm;
    target.track_score = source.track_score;
    target.exit_zone_mask = source.exit_zone_mask;
    target.exit_age_ms = source.exit_age_ms;
    target.exit_recent = source.exit_recent;
    target.room_state = room_state_(source.room_state);
  }

  observation.evidence.pir_motion = pir_motion;
  observation.evidence.filter_blocked = filter_blocked;
  observation.evidence.tracker_presence = tracker.presence;
  observation.evidence.tracker_motion = tracker.motion;
  observation.evidence.static_radar_available = fusion.static_radar.available;
  observation.evidence.static_radar_presence = fusion.static_radar.presence;
  observation.evidence.static_radar_moving = fusion.static_radar.moving;
  observation.evidence.static_radar_still = fusion.static_radar.still;
  observation.evidence.static_radar_distance_mm = fusion.static_radar.detection_distance_mm;
  observation.evidence.static_assist_armed = fusion.static_assist_armed;
  observation.evidence.static_assist_active = fusion.static_assist_active;
  observation.evidence.static_assist_exit_veto = fusion.static_assist_exit_veto;

  this->output_ = observation;
}

LocalObservationRoomState LocalObservationAdapter::room_state_(const char *value) {
  if (value == nullptr)
    return LocalObservationRoomState::UNKNOWN;
  if (std::strcmp(value, "inside") == 0)
    return LocalObservationRoomState::INSIDE;
  if (std::strcmp(value, "outside") == 0)
    return LocalObservationRoomState::OUTSIDE;
  return LocalObservationRoomState::UNKNOWN;
}

uint32_t LocalObservationAdapter::allocate_sequence_() {
  const uint32_t sequence = this->next_sequence_;
  this->next_sequence_++;
  if (this->next_sequence_ == 0)
    this->next_sequence_ = 1;
  return sequence;
}

}  // namespace radar_api_server
}  // namespace esphome
