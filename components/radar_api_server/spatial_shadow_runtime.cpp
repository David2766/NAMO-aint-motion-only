#include "spatial_shadow_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr float PI = 3.14159265358979323846f;

uint8_t bounded_quality(int value) {
  return static_cast<uint8_t>(std::max(0, std::min(100, value)));
}

void rotate_velocity(const SpatialTransform2D &transform, float local_x,
                     float local_y, float *world_x, float *world_y) {
  const float angle =
      static_cast<float>(transform.rotation_cdeg) * 0.01f * PI / 180.0f;
  const float cosine = std::cos(angle);
  const float sine = std::sin(angle);
  *world_x = local_x * cosine + local_y * sine;
  *world_y = local_x * sine - local_y * cosine;
}

SpatialShadowTrackInput transformed_track(
    const LocalObservationTrack &track, const SpatialTransform2D &transform) {
  SpatialShadowTrackInput output;
  if (!track.valid || !valid_spatial_transform(transform))
    return output;
  const auto position =
      apply_spatial_transform(transform, track.x_mm, track.y_mm);
  output.valid = true;
  output.velocity_available = true;
  output.uncertainty_available =
      track.position_variance_x_mm2 > 0.0f &&
      track.position_variance_y_mm2 > 0.0f;
  output.local_track_id = track.local_track_id;
  output.sample_age_ms = track.sample_age_ms;
  output.x_mm = position.x;
  output.y_mm = position.y;
  rotate_velocity(transform, track.vx_mm_s, track.vy_mm_s, &output.vx_mm_s,
                  &output.vy_mm_s);
  output.variance_x_mm2 = track.position_variance_x_mm2;
  output.variance_y_mm2 = track.position_variance_y_mm2;
  output.quality_score = bounded_quality(track.track_score);
  return output;
}

SpatialShadowTrackInput transformed_track(
    const ObservationTransportTrack &track,
    const SpatialTransform2D &transform) {
  SpatialShadowTrackInput output;
  if (!track.valid || !valid_spatial_transform(transform))
    return output;
  const auto position =
      apply_spatial_transform(transform, track.x_mm, track.y_mm);
  output.valid = true;
  output.velocity_available = track.velocity_available;
  output.uncertainty_available = track.uncertainty_available;
  output.local_track_id = track.local_track_id;
  output.sample_age_ms = track.sample_age_ms;
  output.x_mm = position.x;
  output.y_mm = position.y;
  if (track.velocity_available) {
    rotate_velocity(transform, track.vx_mm_s, track.vy_mm_s,
                    &output.vx_mm_s, &output.vy_mm_s);
  }
  output.variance_x_mm2 = track.variance_x_mm2;
  output.variance_y_mm2 = track.variance_y_mm2;
  output.quality_score = track.quality_score;
  return output;
}

}  // namespace

const char *spatial_shadow_runtime_state_to_string(
    SpatialShadowRuntimeState state) {
  switch (state) {
    case SpatialShadowRuntimeState::DISABLED: return "disabled";
    case SpatialShadowRuntimeState::GROUP_UNAVAILABLE:
      return "group_unavailable";
    case SpatialShadowRuntimeState::NOT_GROUP_LEADER:
      return "not_group_leader";
    case SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED:
      return "source_count_unsupported";
    case SpatialShadowRuntimeState::LOCAL_TRANSFORM_UNAVAILABLE:
      return "local_transform_unavailable";
    case SpatialShadowRuntimeState::LOCAL_POSE_UNAVAILABLE:
      return "local_pose_unavailable";
    case SpatialShadowRuntimeState::LOCAL_OBSERVATION_UNAVAILABLE:
      return "local_observation_unavailable";
    case SpatialShadowRuntimeState::READINESS_MISMATCH:
      return "readiness_mismatch";
    case SpatialShadowRuntimeState::REMOTE_OBSERVATION_UNAVAILABLE:
      return "remote_observation_unavailable";
    case SpatialShadowRuntimeState::ACTIVE: return "active";
  }
  return "unknown";
}

uint64_t spatial_shadow_participant_token(const char *participant_id) {
  if (participant_id == nullptr || participant_id[0] == '\0')
    return 0;
  uint64_t hash = 1469598103934665603ULL;
  for (const auto *cursor =
           reinterpret_cast<const unsigned char *>(participant_id);
       *cursor != '\0'; ++cursor) {
    hash ^= static_cast<uint64_t>(*cursor);
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

void SpatialShadowRuntime::update(uint32_t now_ms,
                                  const SpatialShadowRuntimeInput &input) {
  if (!input.enabled) {
    this->pause_(now_ms, SpatialShadowRuntimeState::DISABLED, input);
    return;
  }
  if (!input.group_configured || input.group_id == nullptr ||
      input.group_id[0] == '\0') {
    this->pause_(now_ms, SpatialShadowRuntimeState::GROUP_UNAVAILABLE, input);
    return;
  }
  if (!input.local_group_leader) {
    this->hard_stop_(SpatialShadowRuntimeState::NOT_GROUP_LEADER, input);
    return;
  }
  if (!input.source_count_supported) {
    this->hard_stop_(SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED,
                     input);
    return;
  }
  if (!input.local_transform_authoritative) {
    this->pause_(now_ms,
                 SpatialShadowRuntimeState::LOCAL_TRANSFORM_UNAVAILABLE,
                 input);
    return;
  }
  if (!valid_spatial_transform(input.local_transform)) {
    this->pause_(now_ms, SpatialShadowRuntimeState::LOCAL_POSE_UNAVAILABLE,
                 input);
    return;
  }
  if (input.local_pose_version == 0 ||
      input.expected_local_pose_version != input.local_pose_version) {
    this->pause_(now_ms, SpatialShadowRuntimeState::READINESS_MISMATCH, input);
    return;
  }
  if (input.remote_count > input.remotes.size()) {
    this->hard_stop_(SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED,
                     input);
    return;
  }
  for (std::size_t index = 0; index < input.remote_count; ++index) {
    const auto &remote = input.remotes[index];
    if (remote.participant_token == 0)
      continue;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (input.remotes[previous].participant_token ==
              remote.participant_token &&
          remote.participant_token != 0) {
        this->hard_stop_(SpatialShadowRuntimeState::READINESS_MISMATCH,
                         input);
        return;
      }
    }
  }

  this->begin_active_scope_(input);
  this->diagnostics_.remote_participant_id[0] = '\0';

  auto &sources = this->source_scratch_;
  sources = {};
  this->source_sequence_scratch_ = {};
  uint8_t current_source_count = 0;
  bool local_source_valid = false;
  if (input.local_observation != nullptr) {
    const auto local = local_source_(now_ms, input);
    if (local.valid) {
      local_source_valid = true;
      sources[current_source_count] = local;
      this->source_sequence_scratch_[current_source_count] =
          input.local_observation->sequence;
      current_source_count++;
    }
  }
  for (std::size_t index = 0; index < input.remote_count; ++index) {
    const auto &remote_input = input.remotes[index];
    if (remote_input.observation == nullptr ||
        remote_input.participant_token == 0 ||
        remote_input.expected_pose_version == 0 ||
        remote_input.expected_calibration_version == 0 ||
        remote_input.observation->pose_version !=
            remote_input.expected_pose_version ||
        remote_input.observation->calibration_version !=
            remote_input.expected_calibration_version ||
        !valid_spatial_transform(remote_input.transform))
      continue;
    const auto remote = remote_source_(remote_input);
    if (!remote.valid)
      continue;
    if (current_source_count >= sources.size()) {
      this->hard_stop_(SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED,
                       input);
      return;
    }
    sources[current_source_count] = remote;
    this->source_sequence_scratch_[current_source_count] =
        remote_input.observation->sequence;
    if (this->diagnostics_.remote_participant_id[0] == '\0') {
      copy_(this->diagnostics_.remote_participant_id,
            sizeof(this->diagnostics_.remote_participant_id),
            remote_input.participant_id);
    }
    current_source_count++;
  }

  if (current_source_count == 0) {
    this->pause_(now_ms,
                 !local_source_valid
                     ? SpatialShadowRuntimeState::LOCAL_OBSERVATION_UNAVAILABLE
                     : SpatialShadowRuntimeState::REMOTE_OBSERVATION_UNAVAILABLE,
                 input);
    return;
  }

  if (local_source_valid) {
    bool local_token_collision = false;
    for (std::size_t index = 1; index < current_source_count; ++index) {
      if (sources[0].participant_token == sources[index].participant_token) {
        local_token_collision = true;
        break;
      }
    }
    if (local_token_collision) {
      sources[0].participant_token ^=
          0x8000000000000000ULL ^ static_cast<uint64_t>(current_source_count);
      if (sources[0].participant_token == 0)
        sources[0].participant_token = 1;
    }
  }

  this->diagnostics_.state = SpatialShadowRuntimeState::ACTIVE;
  copy_(this->diagnostics_.group_id, sizeof(this->diagnostics_.group_id),
        input.group_id);
  this->transient_started_ms_ = 0;

  bool unchanged = current_source_count == this->last_source_count_;
  for (std::size_t index = 0; index < current_source_count; ++index) {
    const uint32_t sequence = this->source_sequence_scratch_[index];
    unchanged =
        unchanged &&
        this->last_source_tokens_[index] == sources[index].participant_token &&
        this->last_source_sequences_[index] == sequence;
  }
  if (unchanged) {
    this->diagnostics_.skipped++;
    return;
  }

  this->fusion_.update(now_ms, sources);
  this->last_source_count_ = current_source_count;
  this->last_source_tokens_ = {};
  this->last_source_sequences_ = {};
  for (std::size_t index = 0; index < current_source_count; ++index) {
    this->last_source_tokens_[index] = sources[index].participant_token;
    this->last_source_sequences_[index] =
        this->source_sequence_scratch_[index];
  }
  this->diagnostics_.updates++;
}

void SpatialShadowRuntime::clear() {
  this->fusion_.clear();
  this->source_scratch_ = {};
  this->diagnostics_ = {};
  this->last_source_count_ = 0;
  this->last_source_tokens_ = {};
  this->last_source_sequences_ = {};
  this->source_sequence_scratch_ = {};
  this->active_group_id_[0] = '\0';
  this->active_local_pose_version_ = 0;
  this->active_local_transform_ = {};
  this->transient_started_ms_ = 0;
}

void SpatialShadowRuntime::record_cycle_us(uint32_t cycle_us) {
  this->diagnostics_.max_cycle_us =
      std::max(this->diagnostics_.max_cycle_us, cycle_us);
}

void SpatialShadowRuntime::hard_stop_(SpatialShadowRuntimeState state,
                                      const SpatialShadowRuntimeInput &input) {
  this->fusion_.clear();
  this->diagnostics_.state = state;
  copy_(this->diagnostics_.group_id, sizeof(this->diagnostics_.group_id),
        input.group_id);
  copy_(this->diagnostics_.remote_participant_id,
        sizeof(this->diagnostics_.remote_participant_id),
        input.remote_count > 0 ? input.remotes[0].participant_id : nullptr);
  this->last_source_count_ = 0;
  this->last_source_tokens_ = {};
  this->last_source_sequences_ = {};
  this->active_group_id_[0] = '\0';
  this->active_local_pose_version_ = 0;
  this->active_local_transform_ = {};
  this->transient_started_ms_ = 0;
  this->diagnostics_.skipped++;
}

void SpatialShadowRuntime::pause_(uint32_t now_ms,
                                  SpatialShadowRuntimeState state,
                                  const SpatialShadowRuntimeInput &input) {
  if (this->transient_started_ms_ == 0)
    this->transient_started_ms_ = now_ms == 0 ? 1 : now_ms;
  if (now_ms - this->transient_started_ms_ > TRANSIENT_HOLD_MS) {
    this->fusion_.clear();
    this->last_source_count_ = 0;
    this->last_source_tokens_ = {};
    this->last_source_sequences_ = {};
  }
  this->diagnostics_.state = state;
  copy_(this->diagnostics_.group_id, sizeof(this->diagnostics_.group_id),
        input.group_id);
  copy_(this->diagnostics_.remote_participant_id,
        sizeof(this->diagnostics_.remote_participant_id),
        input.remote_count > 0 ? input.remotes[0].participant_id : nullptr);
  this->diagnostics_.skipped++;
}

void SpatialShadowRuntime::begin_active_scope_(
    const SpatialShadowRuntimeInput &input) {
  const bool group_changed =
      this->active_group_id_[0] != '\0' &&
      std::strcmp(this->active_group_id_, input.group_id) != 0;
  const bool pose_changed =
      this->active_local_pose_version_ != 0 &&
      (this->active_local_pose_version_ != input.local_pose_version ||
       this->active_local_transform_.origin_x_mm !=
           input.local_transform.origin_x_mm ||
       this->active_local_transform_.origin_y_mm !=
           input.local_transform.origin_y_mm ||
       this->active_local_transform_.rotation_cdeg !=
           input.local_transform.rotation_cdeg);
  if (group_changed || pose_changed) {
    this->fusion_.clear();
    this->last_source_count_ = 0;
    this->last_source_tokens_ = {};
    this->last_source_sequences_ = {};
  }
  copy_(this->active_group_id_, sizeof(this->active_group_id_),
        input.group_id);
  this->active_local_pose_version_ = input.local_pose_version;
  this->active_local_transform_ = input.local_transform;
}

SpatialShadowSource SpatialShadowRuntime::local_source_(
    uint32_t now_ms, const SpatialShadowRuntimeInput &input) {
  SpatialShadowSource source;
  const auto &observation = *input.local_observation;
  const uint32_t sample_age_ms = now_ms - observation.observed_at_ms;
  if (observation.sequence == 0 || observation.ttl_ms < 100 ||
      sample_age_ms > observation.ttl_ms)
    return source;
  source.valid = true;
  source.participant_token =
      spatial_shadow_participant_token(input.local_participant_id);
  if (source.participant_token == 0)
    source.participant_token = 1;
  source.view_angle_cdeg = input.local_transform.rotation_cdeg;
  source.sample_age_ms = sample_age_ms;
  source.ttl_ms = observation.ttl_ms;
  for (std::size_t index = 0;
       index < observation.track_count && index < observation.tracks.size();
       ++index) {
    const auto track =
        transformed_track(observation.tracks[index], input.local_transform);
    if (track.valid)
      source.tracks[source.track_count++] = track;
  }
  return source;
}

SpatialShadowSource SpatialShadowRuntime::remote_source_(
    const SpatialShadowRemoteInput &input) {
  SpatialShadowSource source;
  const auto &observation = *input.observation;
  if (!observation.occupied || observation.sequence == 0 ||
      observation.sample_age_ms > observation.ttl_ms)
    return source;
  source.valid = true;
  source.participant_token = input.participant_token;
  source.view_angle_cdeg = input.transform.rotation_cdeg;
  source.sample_age_ms = observation.sample_age_ms;
  source.ttl_ms = observation.ttl_ms;
  for (std::size_t index = 0;
       index < observation.track_count && index < observation.tracks.size();
       ++index) {
    const auto track =
        transformed_track(observation.tracks[index], input.transform);
    if (track.valid)
      source.tracks[source.track_count++] = track;
  }
  return source;
}

void SpatialShadowRuntime::copy_(char *target, std::size_t capacity,
                                 const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  if (source == nullptr) {
    target[0] = '\0';
    return;
  }
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

}  // namespace radar_api_server
}  // namespace esphome
