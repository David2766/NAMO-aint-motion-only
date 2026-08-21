#include "spatial_shadow_fusion.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace esphome {
namespace radar_api_server {

const char *spatial_shadow_track_state_to_string(
    SpatialShadowTrackState state) {
  switch (state) {
    case SpatialShadowTrackState::EMPTY: return "empty";
    case SpatialShadowTrackState::TENTATIVE: return "tentative";
    case SpatialShadowTrackState::CONFIRMED: return "confirmed";
    case SpatialShadowTrackState::COASTING: return "coasting";
  }
  return "unknown";
}

namespace {

constexpr float MAX_COORDINATE_MM = 100000.0f;
constexpr float COST_EPSILON = 0.0001f;

bool finite_in_range(float value, float minimum, float maximum) {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

float square(float value) { return value * value; }

float distance_between(float first_x, float first_y, float second_x,
                       float second_y) {
  return std::sqrt(square(first_x - second_x) + square(first_y - second_y));
}

float bounded_variance(float value, const SpatialShadowFusionPolicy &policy) {
  if (!std::isfinite(value))
    return policy.default_variance_mm2;
  return std::max(policy.minimum_variance_mm2, value);
}

float source_sigma(const SpatialShadowTrackInput &track,
                   const SpatialShadowFusionPolicy &policy) {
  const float variance_x =
      track.uncertainty_available
          ? bounded_variance(track.variance_x_mm2, policy)
          : policy.default_variance_mm2;
  const float variance_y =
      track.uncertainty_available
          ? bounded_variance(track.variance_y_mm2, policy)
          : policy.default_variance_mm2;
  return std::sqrt((variance_x + variance_y) * 0.5f);
}

bool valid_track(const SpatialShadowSource &source,
                 const SpatialShadowTrackInput &track) {
  if (!track.valid || track.local_track_id == 0 ||
      track.sample_age_ms > source.ttl_ms ||
      !finite_in_range(track.x_mm, -MAX_COORDINATE_MM, MAX_COORDINATE_MM) ||
      !finite_in_range(track.y_mm, -MAX_COORDINATE_MM, MAX_COORDINATE_MM) ||
      track.quality_score > 100)
    return false;
  if (track.velocity_available &&
      (!finite_in_range(track.vx_mm_s, -20000.0f, 20000.0f) ||
       !finite_in_range(track.vy_mm_s, -20000.0f, 20000.0f)))
    return false;
  if (track.uncertainty_available &&
      (!finite_in_range(track.variance_x_mm2, 0.0f, 1.0e10f) ||
       !finite_in_range(track.variance_y_mm2, 0.0f, 1.0e10f)))
    return false;
  return true;
}

bool valid_source(const SpatialShadowSource &source) {
  return source.valid && source.participant_token != 0 &&
         source.ttl_ms >= 100 && source.ttl_ms <= 10000 &&
         source.sample_age_ms <= source.ttl_ms &&
         source.track_count <= source.tracks.size() &&
         source.view_angle_cdeg >= -18000 &&
         source.view_angle_cdeg <= 18000;
}

int32_t wrapped_angle_difference_cdeg(int16_t first, int16_t second) {
  int32_t difference =
      std::abs(static_cast<int32_t>(first) - static_cast<int32_t>(second));
  if (difference > 18000)
    difference = 36000 - difference;
  return difference;
}

float pair_gate(const SpatialShadowTrackInput &first,
                const SpatialShadowTrackInput &second,
                const SpatialShadowFusionPolicy &policy) {
  const float combined_sigma =
      std::sqrt(square(source_sigma(first, policy)) +
                square(source_sigma(second, policy)));
  return std::min(policy.max_pair_gate_mm,
                  policy.base_pair_gate_mm +
                      policy.uncertainty_gate_multiplier * combined_sigma);
}

float pair_cost(const SpatialShadowTrackInput &first,
                const SpatialShadowTrackInput &second,
                const SpatialShadowFusionPolicy &policy, float gate,
                bool *accepted) {
  *accepted = false;
  const float distance =
      distance_between(first.x_mm, first.y_mm, second.x_mm, second.y_mm);
  if (distance > gate)
    return 0.0f;

  float cost = distance / std::max(1.0f, gate);
  if (first.velocity_available && second.velocity_available) {
    const float velocity_difference =
        distance_between(first.vx_mm_s, first.vy_mm_s, second.vx_mm_s,
                         second.vy_mm_s);
    if (velocity_difference > policy.max_velocity_difference_mm_s)
      return 0.0f;
    cost += 0.25f * velocity_difference /
            std::max(1.0f, policy.max_velocity_difference_mm_s);
  }
  *accepted = true;
  return cost;
}

float inverse_variance_weight(float variance,
                              const SpatialShadowFusionPolicy &policy) {
  return 1.0f / bounded_variance(variance, policy);
}

float weighted_value(float first, float first_weight, float second,
                     float second_weight) {
  const float total = first_weight + second_weight;
  return total > 0.0f ? (first * first_weight + second * second_weight) / total
                      : (first + second) * 0.5f;
}

}  // namespace

SpatialShadowFusion::SpatialShadowFusion(
    const SpatialShadowFusionPolicy &policy)
    : policy_(policy) {}

void SpatialShadowFusion::clear() {
  this->tracks_ = {};
  this->measurements_ = {};
  this->pair_candidates_ = {};
  this->lifecycle_candidates_ = {};
  this->output_ = {};
  // IDs and output sequences are scoped to this runtime instance. Keeping
  // them monotonic prevents a temporary input outage from making resumed
  // fusion output look stale to downstream consumers.
}

void SpatialShadowFusion::update(
    uint32_t now_ms,
    const std::array<SpatialShadowSource, MAX_SOURCES> &sources) {
  SpatialShadowFusionDiagnostics diagnostics;
  const uint8_t measurement_count =
      this->build_measurements_(sources, &diagnostics);
  this->associate_measurements_(now_ms, measurement_count, &diagnostics);
  this->publish_(now_ms, diagnostics);
}

uint8_t SpatialShadowFusion::build_measurements_(
    const std::array<SpatialShadowSource, MAX_SOURCES> &sources,
    SpatialShadowFusionDiagnostics *diagnostics) {
  this->measurements_ = {};
  this->pair_candidates_ = {};

  std::array<std::array<bool, SpatialShadowSource::MAX_TRACKS>, MAX_SOURCES>
      valid{};
  std::array<uint8_t, MAX_SOURCES> valid_track_counts{};
  std::array<uint8_t, MAX_SOURCES> source_order{};
  uint8_t source_count = 0;
  for (std::size_t source_index = 0; source_index < sources.size();
       ++source_index) {
    const auto &source = sources[source_index];
    if (!valid_source(source))
      continue;
    bool duplicate = false;
    for (std::size_t existing = 0; existing < source_count; ++existing) {
      if (sources[source_order[existing]].participant_token ==
          source.participant_token) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;
    source_order[source_count++] = static_cast<uint8_t>(source_index);
    diagnostics->valid_inputs++;
    for (std::size_t track_index = 0; track_index < source.track_count;
         ++track_index)
      valid[source_index][track_index] =
          valid_track(source, source.tracks[track_index]);
    for (std::size_t track_index = 0; track_index < source.track_count;
         ++track_index) {
      if (valid[source_index][track_index])
        valid_track_counts[source_index]++;
    }
  }

  std::sort(source_order.begin(), source_order.begin() + source_count,
            [&sources](uint8_t first, uint8_t second) {
              return sources[first].participant_token <
                     sources[second].participant_token;
            });

  uint8_t measurement_count = 0;
  for (std::size_t ordered_index = 0; ordered_index < source_count;
       ++ordered_index) {
    const std::size_t source_index = source_order[ordered_index];
    const auto &source = sources[source_index];
    std::array<bool, MAX_MEASUREMENTS> measurement_matched{};
    std::array<bool, SpatialShadowSource::MAX_TRACKS> source_matched{};

    this->pair_candidates_ = {};
    std::array<float, MAX_MEASUREMENTS> measurement_best{};
    std::array<float, MAX_MEASUREMENTS> measurement_second{};
    std::array<float, SpatialShadowSource::MAX_TRACKS> source_best{};
    std::array<float, SpatialShadowSource::MAX_TRACKS> source_second{};
    measurement_best.fill(std::numeric_limits<float>::infinity());
    measurement_second.fill(std::numeric_limits<float>::infinity());
    source_best.fill(std::numeric_limits<float>::infinity());
    source_second.fill(std::numeric_limits<float>::infinity());

    std::size_t candidate_count = 0;
    for (std::size_t measurement_index = 0;
         measurement_index < measurement_count; ++measurement_index) {
      const auto &measurement = this->measurements_[measurement_index];
      SpatialShadowTrackInput aggregate;
      aggregate.valid = true;
      aggregate.velocity_available = measurement.velocity_available;
      aggregate.uncertainty_available = true;
      aggregate.local_track_id = 1;
      aggregate.x_mm = measurement.x_mm;
      aggregate.y_mm = measurement.y_mm;
      aggregate.vx_mm_s = measurement.vx_mm_s;
      aggregate.vy_mm_s = measurement.vy_mm_s;
      aggregate.variance_x_mm2 = measurement.variance_x_mm2;
      aggregate.variance_y_mm2 = measurement.variance_y_mm2;
      aggregate.quality_score = measurement.confidence;

      for (std::size_t track_index = 0;
           track_index < source.track_count; ++track_index) {
        if (!valid[source_index][track_index])
          continue;
        const auto &track = source.tracks[track_index];
        float gate = pair_gate(aggregate, track, this->policy_);
        if (this->established_measurement_matches_(
                measurement, source.participant_token,
                track.local_track_id)) {
          gate += this->policy_.pair_release_margin_mm;
        }
        bool accepted = false;
        const float cost =
            pair_cost(aggregate, track, this->policy_, gate, &accepted);
        if (!accepted)
          continue;
        auto &candidate = this->pair_candidates_[candidate_count++];
        candidate = {true, static_cast<uint8_t>(measurement_index),
                     static_cast<uint8_t>(track_index), cost};
        diagnostics->candidate_pairs++;
        if (cost < measurement_best[measurement_index]) {
          measurement_second[measurement_index] =
              measurement_best[measurement_index];
          measurement_best[measurement_index] = cost;
        } else if (cost < measurement_second[measurement_index]) {
          measurement_second[measurement_index] = cost;
        }
        if (cost < source_best[track_index]) {
          source_second[track_index] = source_best[track_index];
          source_best[track_index] = cost;
        } else if (cost < source_second[track_index]) {
          source_second[track_index] = cost;
        }
      }
    }

    std::sort(this->pair_candidates_.begin(),
              this->pair_candidates_.begin() + candidate_count,
              pair_candidate_less_);
    for (std::size_t candidate_index = 0;
         candidate_index < candidate_count; ++candidate_index) {
      const auto &candidate = this->pair_candidates_[candidate_index];
      if (measurement_matched[candidate.first] ||
          source_matched[candidate.second])
        continue;
      const bool measurement_ambiguous =
          std::isfinite(measurement_second[candidate.first]) &&
          measurement_second[candidate.first] - candidate.cost <
              this->policy_.ambiguity_margin;
      const bool source_ambiguous =
          std::isfinite(source_second[candidate.second]) &&
          source_second[candidate.second] - candidate.cost <
              this->policy_.ambiguity_margin;
      if (measurement_ambiguous || source_ambiguous) {
        diagnostics->ambiguous_pairs++;
        continue;
      }
      this->merge_measurement_(
          &this->measurements_[candidate.first], source,
          source.tracks[candidate.second]);
      this->measurements_[candidate.first].source_had_multiple_tracks =
          this->measurements_[candidate.first].source_had_multiple_tracks ||
          valid_track_counts[source_index] > 1;
      measurement_matched[candidate.first] = true;
      source_matched[candidate.second] = true;
      diagnostics->paired_measurements++;
    }

    for (std::size_t track_index = 0; track_index < source.track_count;
         ++track_index) {
      if (!valid[source_index][track_index] || source_matched[track_index])
        continue;
      if (measurement_count >= this->measurements_.size()) {
        diagnostics->dropped_measurements++;
        continue;
      }
      this->seed_measurement_(&this->measurements_[measurement_count++],
                              source, source.tracks[track_index]);
      this->measurements_[measurement_count - 1].source_had_multiple_tracks =
          valid_track_counts[source_index] > 1;
    }
  }

  return measurement_count;
}

void SpatialShadowFusion::associate_measurements_(
    uint32_t now_ms, uint8_t measurement_count,
    SpatialShadowFusionDiagnostics *diagnostics) {
  this->lifecycle_candidates_ = {};
  std::array<bool, SpatialShadowFusionOutput::MAX_TRACKS> track_matched{};
  std::array<bool, MAX_MEASUREMENTS> measurement_matched{};
  std::size_t candidate_count = 0;
  bool established_track_exists = false;
  for (const auto &track : this->tracks_) {
    if (track.occupied &&
        (track.state == SpatialShadowTrackState::CONFIRMED ||
         track.state == SpatialShadowTrackState::COASTING)) {
      established_track_exists = true;
      break;
    }
  }

  for (std::size_t track_index = 0; track_index < this->tracks_.size();
       ++track_index) {
    const auto &track = this->tracks_[track_index];
    if (!track.occupied)
      continue;
    const uint32_t elapsed_ms = now_ms - track.last_update_ms;
    const float prediction_seconds =
        static_cast<float>(std::min<uint32_t>(elapsed_ms, 1000)) / 1000.0f;
    const float speed =
        std::sqrt(square(track.value.vx_mm_s) + square(track.value.vy_mm_s));
    const float speed_scale =
        speed > this->policy_.max_predicted_speed_mm_s && speed > 0.0f
            ? this->policy_.max_predicted_speed_mm_s / speed
            : 1.0f;
    const float predicted_x =
        track.value.x_mm +
        track.value.vx_mm_s * speed_scale * prediction_seconds;
    const float predicted_y =
        track.value.y_mm +
        track.value.vy_mm_s * speed_scale * prediction_seconds;

    for (std::size_t measurement_index = 0;
         measurement_index < measurement_count; ++measurement_index) {
      const auto &measurement = this->measurements_[measurement_index];
      const bool continuity =
          track_identity_matches_(track, measurement);
      const float gate =
          continuity ? this->policy_.lifecycle_continuity_gate_mm
                     : this->policy_.lifecycle_gate_mm;
      const float distance =
          distance_between(predicted_x, predicted_y, measurement.x_mm,
                           measurement.y_mm);
      if (distance > gate)
        continue;
      float cost = distance / std::max(1.0f, gate);
      if (continuity)
        cost -= 0.5f;
      auto &candidate = this->lifecycle_candidates_[candidate_count++];
      candidate = {true, static_cast<uint8_t>(track_index),
                   static_cast<uint8_t>(measurement_index), cost};
    }
  }

  std::sort(this->lifecycle_candidates_.begin(),
            this->lifecycle_candidates_.begin() + candidate_count,
            lifecycle_candidate_less_);

  for (std::size_t index = 0; index < candidate_count; ++index) {
    const auto &candidate = this->lifecycle_candidates_[index];
    if (track_matched[candidate.track] ||
        measurement_matched[candidate.measurement])
      continue;
    auto &track = this->tracks_[candidate.track];
    this->smooth_track_measurement_(
        &track, this->measurements_[candidate.measurement], now_ms);
    track.reanchor_pending = false;
    track.reanchor_hits = 0;
    track.reanchor_last_ms = 0;
    remember_measurement_identity_(&track, track.value);
    if (track.value.source_count >= 2) {
      track.secondary_guarded = false;
      track.secondary_independent = true;
    } else {
      this->update_secondary_evidence_(&track);
    }
    track.confirmation_hits_required =
        std::min(track.confirmation_hits_required,
                 this->confirmation_hits_for_(track.value,
                                              established_track_exists));
    track.hit_count =
        track.hit_count == std::numeric_limits<uint16_t>::max()
            ? track.hit_count
            : static_cast<uint16_t>(track.hit_count + 1);
    track.last_seen_ms = now_ms;
    track.last_update_ms = now_ms;
    if (track.state == SpatialShadowTrackState::COASTING ||
        this->should_confirm_track_(track, now_ms))
      track.state = SpatialShadowTrackState::CONFIRMED;
    track_matched[candidate.track] = true;
    measurement_matched[candidate.measurement] = true;
  }

  for (std::size_t track_index = 0; track_index < this->tracks_.size();
       ++track_index) {
    auto &track = this->tracks_[track_index];
    if (!track.occupied || track_matched[track_index])
      continue;
    const uint32_t unseen_ms = now_ms - track.last_seen_ms;
    const bool corroborated = track.identity_count >= 2;
    const uint32_t hold_ms =
        track.state == SpatialShadowTrackState::TENTATIVE || !corroborated
            ? this->policy_.tentative_hold_ms
            : this->policy_.confirmed_hold_ms;
    if (unseen_ms > hold_ms) {
      track = TrackSlot{};
      continue;
    }
    if (track.state == SpatialShadowTrackState::CONFIRMED)
      track.state = SpatialShadowTrackState::COASTING;
    const uint32_t elapsed_ms = now_ms - track.last_update_ms;
    const float prediction_seconds =
        static_cast<float>(std::min<uint32_t>(elapsed_ms, 1000)) / 1000.0f;
    const float speed =
        std::sqrt(square(track.value.vx_mm_s) + square(track.value.vy_mm_s));
    const float speed_scale =
        speed > this->policy_.max_predicted_speed_mm_s && speed > 0.0f
            ? this->policy_.max_predicted_speed_mm_s / speed
            : 1.0f;
    track.value.x_mm +=
        track.value.vx_mm_s * speed_scale * prediction_seconds;
    track.value.y_mm +=
        track.value.vy_mm_s * speed_scale * prediction_seconds;
    track.last_update_ms = now_ms;
  }

  for (std::size_t measurement_index = 0;
       measurement_index < measurement_count; ++measurement_index) {
    if (measurement_matched[measurement_index])
      continue;
    TrackSlot *established_identity = nullptr;
    for (auto &existing : this->tracks_) {
      if (!existing.occupied ||
          (existing.state != SpatialShadowTrackState::CONFIRMED &&
           existing.state != SpatialShadowTrackState::COASTING))
        continue;
      if (track_identity_matches_(existing,
                                  this->measurements_[measurement_index])) {
        established_identity = &existing;
        break;
      }
    }
    if (established_identity != nullptr) {
      if (this->consider_reanchor_(
              established_identity, this->measurements_[measurement_index],
              now_ms)) {
        measurement_matched[measurement_index] = true;
        continue;
      }
      diagnostics->dropped_measurements++;
      continue;
    }
    bool belongs_to_established_source = false;
    if (!this->measurements_[measurement_index].source_had_multiple_tracks) {
      for (const auto &existing : this->tracks_) {
        if (!existing.occupied ||
            (existing.state != SpatialShadowTrackState::CONFIRMED &&
             existing.state != SpatialShadowTrackState::COASTING))
          continue;
        if (track_source_matches_(existing,
                                  this->measurements_[measurement_index])) {
          belongs_to_established_source = true;
          break;
        }
      }
    }
    if (belongs_to_established_source) {
      diagnostics->dropped_measurements++;
      continue;
    }
    const TrackSlot *parent =
        this->nearest_reference_track_(this->measurements_[measurement_index]);
    TrackSlot *track = this->allocate_track_();
    if (track == nullptr) {
      diagnostics->dropped_measurements++;
      continue;
    }
    track->occupied = true;
    track->fused_track_id = this->allocate_track_id_();
    track->confirmation_hits_required =
        this->confirmation_hits_for_(this->measurements_[measurement_index],
                                     established_track_exists);
    track->hit_count = 1;
    track->first_seen_ms = now_ms;
    track->last_seen_ms = now_ms;
    track->last_update_ms = now_ms;
    track->value = this->measurements_[measurement_index];
    if (parent != nullptr && track->value.source_count == 1) {
      track->secondary_guarded = true;
      track->secondary_relative_initialized = true;
      track->secondary_parent_track_id = parent->fused_track_id;
      track->secondary_initial_relative_x_mm =
          track->value.x_mm - parent->value.x_mm;
      track->secondary_initial_relative_y_mm =
          track->value.y_mm - parent->value.y_mm;
    }
    track->state = this->should_confirm_track_(*track, now_ms)
                       ? SpatialShadowTrackState::CONFIRMED
                       : SpatialShadowTrackState::TENTATIVE;
    remember_measurement_identity_(track, track->value);
  }
}

void SpatialShadowFusion::publish_(
    uint32_t now_ms, const SpatialShadowFusionDiagnostics &diagnostics) {
  SpatialShadowFusionOutput output;
  output.sequence = this->next_sequence_++;
  if (this->next_sequence_ == 0)
    this->next_sequence_ = 1;
  output.diagnostics = diagnostics;

  for (const auto &source : this->tracks_) {
    if (!source.occupied || output.track_count >= output.tracks.size())
      continue;
    auto &target = output.tracks[output.track_count++];
    target.valid = true;
    target.correlated_views = source.value.correlated_views;
    target.fused_track_id = source.fused_track_id;
    target.state = source.state;
    target.source_count = source.value.source_count;
    target.confidence = source.value.confidence;
    target.hit_count = source.hit_count;
    target.age_ms = now_ms - source.first_seen_ms;
    target.x_mm = source.value.x_mm;
    target.y_mm = source.value.y_mm;
    target.vx_mm_s = source.value.vx_mm_s;
    target.vy_mm_s = source.value.vy_mm_s;
    target.variance_x_mm2 = source.value.variance_x_mm2;
    target.variance_y_mm2 = source.value.variance_y_mm2;
    target.participant_tokens = source.value.participant_tokens;
    target.local_track_ids = source.value.local_track_ids;
    switch (source.state) {
      case SpatialShadowTrackState::TENTATIVE:
        output.diagnostics.tentative_tracks++;
        break;
      case SpatialShadowTrackState::CONFIRMED:
        output.diagnostics.confirmed_tracks++;
        break;
      case SpatialShadowTrackState::COASTING:
        output.diagnostics.coasting_tracks++;
        break;
      case SpatialShadowTrackState::EMPTY:
        break;
    }
  }

  this->output_ = output;
}

SpatialShadowFusion::TrackSlot *SpatialShadowFusion::allocate_track_() {
  for (auto &track : this->tracks_) {
    if (!track.occupied)
      return &track;
  }
  return nullptr;
}

uint32_t SpatialShadowFusion::allocate_track_id_() {
  const uint32_t result = this->next_track_id_;
  this->next_track_id_++;
  if (this->next_track_id_ == 0)
    this->next_track_id_ = 1;
  return result;
}

bool SpatialShadowFusion::pair_candidate_less_(const PairCandidate &first,
                                               const PairCandidate &second) {
  if (std::fabs(first.cost - second.cost) > COST_EPSILON)
    return first.cost < second.cost;
  if (first.first != second.first)
    return first.first < second.first;
  return first.second < second.second;
}

bool SpatialShadowFusion::lifecycle_candidate_less_(
    const LifecycleCandidate &first, const LifecycleCandidate &second) {
  if (std::fabs(first.cost - second.cost) > COST_EPSILON)
    return first.cost < second.cost;
  if (first.track != second.track)
    return first.track < second.track;
  return first.measurement < second.measurement;
}

bool SpatialShadowFusion::track_identity_matches_(
    const TrackSlot &track, const Measurement &measurement) {
  for (std::size_t measurement_index = 0;
       measurement_index < measurement.source_count; ++measurement_index) {
    const uint64_t participant =
        measurement.participant_tokens[measurement_index];
    const uint32_t local_track_id =
        measurement.local_track_ids[measurement_index];
    if (participant == 0 || local_track_id == 0)
      continue;
    for (std::size_t identity_index = 0;
         identity_index < track.identity_count; ++identity_index) {
      if (track.identity_participant_tokens[identity_index] == participant &&
          track.identity_local_track_ids[identity_index] == local_track_id)
        return true;
    }
  }
  return false;
}

bool SpatialShadowFusion::track_source_matches_(
    const TrackSlot &track, const Measurement &measurement) {
  for (std::size_t measurement_index = 0;
       measurement_index < measurement.source_count; ++measurement_index) {
    const uint64_t participant =
        measurement.participant_tokens[measurement_index];
    if (participant == 0)
      continue;
    for (std::size_t identity_index = 0;
         identity_index < track.identity_count; ++identity_index) {
      if (track.identity_participant_tokens[identity_index] == participant)
        return true;
    }
  }
  return false;
}

bool SpatialShadowFusion::established_measurement_matches_(
    const Measurement &measurement, uint64_t participant_token,
    uint32_t local_track_id) const {
  if (participant_token == 0 || local_track_id == 0)
    return false;

  for (const auto &track : this->tracks_) {
    if (!track.occupied ||
        (track.state != SpatialShadowTrackState::CONFIRMED &&
         track.state != SpatialShadowTrackState::COASTING))
      continue;
    bool measurement_matches = false;
    bool source_matches = false;
    for (std::size_t index = 0; index < track.identity_count; ++index) {
      const uint64_t identity_participant =
          track.identity_participant_tokens[index];
      const uint32_t identity_local_track_id =
          track.identity_local_track_ids[index];
      source_matches =
          source_matches ||
          (identity_participant == participant_token &&
           identity_local_track_id == local_track_id);
      for (std::size_t measurement_index = 0;
           measurement_index < measurement.source_count;
           ++measurement_index) {
        measurement_matches =
            measurement_matches ||
            (identity_participant ==
                 measurement.participant_tokens[measurement_index] &&
             identity_local_track_id ==
                 measurement.local_track_ids[measurement_index]);
      }
    }
    if (measurement_matches && source_matches)
      return true;
  }
  return false;
}

void SpatialShadowFusion::seed_measurement_(
    Measurement *measurement, const SpatialShadowSource &source,
    const SpatialShadowTrackInput &track) const {
  if (measurement == nullptr)
    return;
  *measurement = {};
  measurement->valid = true;
  measurement->velocity_available = track.velocity_available;
  measurement->source_count = 1;
  measurement->confidence = track.quality_score;
  measurement->x_mm = track.x_mm;
  measurement->y_mm = track.y_mm;
  measurement->vx_mm_s = track.velocity_available ? track.vx_mm_s : 0.0f;
  measurement->vy_mm_s = track.velocity_available ? track.vy_mm_s : 0.0f;
  measurement->variance_x_mm2 =
      track.uncertainty_available
          ? bounded_variance(track.variance_x_mm2, this->policy_)
          : this->policy_.default_variance_mm2;
  measurement->variance_y_mm2 =
      track.uncertainty_available
          ? bounded_variance(track.variance_y_mm2, this->policy_)
          : this->policy_.default_variance_mm2;
  measurement->participant_tokens[0] = source.participant_token;
  measurement->local_track_ids[0] = track.local_track_id;
  measurement->view_angles_cdeg[0] = source.view_angle_cdeg;
  measurement->source_x_mm[0] = track.x_mm;
  measurement->source_y_mm[0] = track.y_mm;
  measurement->source_vx_mm_s[0] = track.vx_mm_s;
  measurement->source_vy_mm_s[0] = track.vy_mm_s;
  measurement->source_variance_x_mm2[0] = measurement->variance_x_mm2;
  measurement->source_variance_y_mm2[0] = measurement->variance_y_mm2;
  measurement->source_velocity_available[0] = track.velocity_available;
}

void SpatialShadowFusion::merge_measurement_(
    Measurement *measurement, const SpatialShadowSource &source,
    const SpatialShadowTrackInput &track) const {
  if (measurement == nullptr || !measurement->valid ||
      measurement->source_count >= measurement->participant_tokens.size())
    return;

  bool correlated = false;
  for (std::size_t index = 0; index < measurement->source_count; ++index) {
    if (wrapped_angle_difference_cdeg(measurement->view_angles_cdeg[index],
                                     source.view_angle_cdeg) <=
        this->policy_.correlated_view_angle_cdeg) {
      correlated = true;
      break;
    }
  }

  const float measurement_variance_x =
      bounded_variance(measurement->variance_x_mm2, this->policy_);
  const float measurement_variance_y =
      bounded_variance(measurement->variance_y_mm2, this->policy_);
  const float source_variance_x =
      track.uncertainty_available
          ? bounded_variance(track.variance_x_mm2, this->policy_)
          : this->policy_.default_variance_mm2;
  const float source_variance_y =
      track.uncertainty_available
          ? bounded_variance(track.variance_y_mm2, this->policy_)
          : this->policy_.default_variance_mm2;
  const float measurement_weight_x =
      inverse_variance_weight(measurement_variance_x, this->policy_);
  const float measurement_weight_y =
      inverse_variance_weight(measurement_variance_y, this->policy_);
  const float source_weight_x =
      inverse_variance_weight(source_variance_x, this->policy_);
  const float source_weight_y =
      inverse_variance_weight(source_variance_y, this->policy_);

  measurement->x_mm =
      weighted_value(measurement->x_mm, measurement_weight_x, track.x_mm,
                     source_weight_x);
  measurement->y_mm =
      weighted_value(measurement->y_mm, measurement_weight_y, track.y_mm,
                     source_weight_y);

  if (correlated) {
    measurement->variance_x_mm2 =
        std::min(measurement_variance_x, source_variance_x);
    measurement->variance_y_mm2 =
        std::min(measurement_variance_y, source_variance_y);
    measurement->confidence =
        std::max(measurement->confidence, track.quality_score);
  } else {
    measurement->variance_x_mm2 =
        1.0f / (measurement_weight_x + source_weight_x);
    measurement->variance_y_mm2 =
        1.0f / (measurement_weight_y + source_weight_y);
    measurement->confidence = static_cast<uint8_t>(std::min(
        100, static_cast<int>(std::max(measurement->confidence,
                                       track.quality_score)) +
                 static_cast<int>(std::min(measurement->confidence,
                                           track.quality_score)) /
                     4));
  }

  if (measurement->velocity_available && track.velocity_available) {
    const float measurement_velocity_weight =
        (measurement_weight_x + measurement_weight_y) * 0.5f;
    const float source_velocity_weight =
        (source_weight_x + source_weight_y) * 0.5f;
    measurement->vx_mm_s =
        weighted_value(measurement->vx_mm_s, measurement_velocity_weight,
                       track.vx_mm_s, source_velocity_weight);
    measurement->vy_mm_s =
        weighted_value(measurement->vy_mm_s, measurement_velocity_weight,
                       track.vy_mm_s, source_velocity_weight);
  } else if (track.velocity_available) {
    measurement->vx_mm_s = track.vx_mm_s;
    measurement->vy_mm_s = track.vy_mm_s;
    measurement->velocity_available = true;
  }

  const std::size_t index = measurement->source_count++;
  measurement->participant_tokens[index] = source.participant_token;
  measurement->local_track_ids[index] = track.local_track_id;
  measurement->view_angles_cdeg[index] = source.view_angle_cdeg;
  measurement->source_x_mm[index] = track.x_mm;
  measurement->source_y_mm[index] = track.y_mm;
  measurement->source_vx_mm_s[index] = track.vx_mm_s;
  measurement->source_vy_mm_s[index] = track.vy_mm_s;
  measurement->source_variance_x_mm2[index] = source_variance_x;
  measurement->source_variance_y_mm2[index] = source_variance_y;
  measurement->source_velocity_available[index] = track.velocity_available;
  measurement->correlated_views =
      measurement->correlated_views || correlated;
  if (measurement->source_count >= 3)
    this->recompute_robust_geometry_(measurement);
}

void SpatialShadowFusion::recompute_robust_geometry_(
    Measurement *measurement) const {
  if (measurement == nullptr || measurement->source_count < 3)
    return;

  std::size_t medoid = 0;
  float medoid_cost = std::numeric_limits<float>::infinity();
  for (std::size_t candidate = 0; candidate < measurement->source_count;
       ++candidate) {
    float cost = 0.0f;
    for (std::size_t other = 0; other < measurement->source_count; ++other) {
      cost += distance_between(measurement->source_x_mm[candidate],
                               measurement->source_y_mm[candidate],
                               measurement->source_x_mm[other],
                               measurement->source_y_mm[other]);
    }
    if (cost < medoid_cost) {
      medoid = candidate;
      medoid_cost = cost;
    }
  }

  std::array<bool, MAX_SOURCES> inlier{};
  std::array<float, MAX_SOURCES> weights_x{};
  std::array<float, MAX_SOURCES> weights_y{};
  std::array<float, MAX_SOURCES> sorted_weights{};
  std::size_t inlier_count = 0;
  for (std::size_t index = 0; index < measurement->source_count; ++index) {
    const float distance =
        distance_between(measurement->source_x_mm[medoid],
                         measurement->source_y_mm[medoid],
                         measurement->source_x_mm[index],
                         measurement->source_y_mm[index]);
    if (index != medoid && distance > this->policy_.robust_outlier_gate_mm)
      continue;
    inlier[index] = true;
    weights_x[index] = inverse_variance_weight(
        measurement->source_variance_x_mm2[index], this->policy_);
    weights_y[index] = inverse_variance_weight(
        measurement->source_variance_y_mm2[index], this->policy_);
    sorted_weights[inlier_count++] =
        std::sqrt(weights_x[index] * weights_y[index]);
  }
  if (inlier_count == 0)
    return;

  std::sort(sorted_weights.begin(), sorted_weights.begin() + inlier_count);
  const float median_weight = sorted_weights[inlier_count / 2];
  const float ratio = std::max(1.0f, this->policy_.max_robust_weight_ratio);
  const float minimum_weight = median_weight / ratio;
  const float maximum_weight = median_weight * ratio;
  float total_weight_x = 0.0f;
  float total_weight_y = 0.0f;
  float weighted_x = 0.0f;
  float weighted_y = 0.0f;
  float velocity_weight = 0.0f;
  float weighted_vx = 0.0f;
  float weighted_vy = 0.0f;
  for (std::size_t index = 0; index < measurement->source_count; ++index) {
    if (!inlier[index])
      continue;
    const float weight_x =
        std::max(minimum_weight, std::min(maximum_weight, weights_x[index]));
    const float weight_y =
        std::max(minimum_weight, std::min(maximum_weight, weights_y[index]));
    total_weight_x += weight_x;
    total_weight_y += weight_y;
    weighted_x += measurement->source_x_mm[index] * weight_x;
    weighted_y += measurement->source_y_mm[index] * weight_y;
    if (measurement->source_velocity_available[index]) {
      const float weight = (weight_x + weight_y) * 0.5f;
      velocity_weight += weight;
      weighted_vx += measurement->source_vx_mm_s[index] * weight;
      weighted_vy += measurement->source_vy_mm_s[index] * weight;
    }
  }
  if (total_weight_x <= 0.0f || total_weight_y <= 0.0f)
    return;

  measurement->x_mm = weighted_x / total_weight_x;
  measurement->y_mm = weighted_y / total_weight_y;
  if (velocity_weight > 0.0f) {
    measurement->velocity_available = true;
    measurement->vx_mm_s = weighted_vx / velocity_weight;
    measurement->vy_mm_s = weighted_vy / velocity_weight;
  }

  bool correlated = false;
  float minimum_variance_x = std::numeric_limits<float>::infinity();
  float minimum_variance_y = std::numeric_limits<float>::infinity();
  for (std::size_t first = 0; first < measurement->source_count; ++first) {
    if (!inlier[first])
      continue;
    minimum_variance_x =
        std::min(minimum_variance_x,
                 measurement->source_variance_x_mm2[first]);
    minimum_variance_y =
        std::min(minimum_variance_y,
                 measurement->source_variance_y_mm2[first]);
    for (std::size_t second = first + 1;
         second < measurement->source_count; ++second) {
      if (inlier[second] &&
          wrapped_angle_difference_cdeg(
              measurement->view_angles_cdeg[first],
              measurement->view_angles_cdeg[second]) <=
              this->policy_.correlated_view_angle_cdeg)
        correlated = true;
    }
  }
  measurement->correlated_views = correlated;
  measurement->variance_x_mm2 =
      correlated ? bounded_variance(minimum_variance_x, this->policy_)
                 : bounded_variance(1.0f / total_weight_x, this->policy_);
  measurement->variance_y_mm2 =
      correlated ? bounded_variance(minimum_variance_y, this->policy_)
                 : bounded_variance(1.0f / total_weight_y, this->policy_);

  measurement->geometry_spread_mm = 0.0f;
  for (std::size_t index = 0; index < measurement->source_count; ++index) {
    if (!inlier[index])
      continue;
    measurement->geometry_spread_mm = std::max(
        measurement->geometry_spread_mm,
        distance_between(measurement->x_mm, measurement->y_mm,
                         measurement->source_x_mm[index],
                         measurement->source_y_mm[index]));
  }
}

void SpatialShadowFusion::smooth_track_measurement_(
    TrackSlot *track, const Measurement &measurement, uint32_t now_ms) const {
  if (track == nullptr)
    return;

  const Measurement previous = track->value;
  const uint32_t elapsed_ms = now_ms - track->last_update_ms;
  const float prediction_seconds =
      static_cast<float>(std::min<uint32_t>(elapsed_ms, 1000)) / 1000.0f;
  const float speed =
      std::sqrt(square(previous.vx_mm_s) + square(previous.vy_mm_s));
  const float speed_scale =
      speed > this->policy_.max_predicted_speed_mm_s && speed > 0.0f
          ? this->policy_.max_predicted_speed_mm_s / speed
          : 1.0f;
  const float predicted_x =
      previous.x_mm +
      previous.vx_mm_s * speed_scale * prediction_seconds;
  const float predicted_y =
      previous.y_mm +
      previous.vy_mm_s * speed_scale * prediction_seconds;

  float position_alpha =
      measurement.source_count >= 2
          ? this->policy_.paired_position_alpha
          : this->policy_.single_source_position_alpha;
  if (previous.source_count >= 2 && measurement.source_count == 1)
    position_alpha = this->policy_.source_dropout_position_alpha;
  const float measurement_speed =
      std::sqrt(square(measurement.vx_mm_s) + square(measurement.vy_mm_s));
  if (measurement.geometry_spread_mm >=
      this->policy_.disagreement_spread_mm) {
    position_alpha =
        std::min(position_alpha, this->policy_.disagreement_position_alpha);
  } else if (track->state != SpatialShadowTrackState::TENTATIVE &&
             speed <= this->policy_.stationary_speed_mm_s &&
             measurement_speed <= this->policy_.stationary_speed_mm_s) {
    position_alpha =
        std::min(position_alpha, this->policy_.stationary_position_alpha);
  }
  position_alpha = std::max(0.0f, std::min(1.0f, position_alpha));
  const float velocity_alpha =
      std::max(0.0f, std::min(1.0f, this->policy_.velocity_alpha));

  track->value = measurement;
  track->value.x_mm =
      predicted_x + (measurement.x_mm - predicted_x) * position_alpha;
  track->value.y_mm =
      predicted_y + (measurement.y_mm - predicted_y) * position_alpha;
  track->value.vx_mm_s =
      previous.vx_mm_s +
      (measurement.vx_mm_s - previous.vx_mm_s) * velocity_alpha;
  track->value.vy_mm_s =
      previous.vy_mm_s +
      (measurement.vy_mm_s - previous.vy_mm_s) * velocity_alpha;
}

bool SpatialShadowFusion::consider_reanchor_(
    TrackSlot *track, const Measurement &measurement, uint32_t now_ms) const {
  if (track == nullptr || !measurement.valid ||
      (track->identity_count >= 2 && measurement.source_count < 2)) {
    if (track != nullptr) {
      track->reanchor_pending = false;
      track->reanchor_hits = 0;
      track->reanchor_last_ms = 0;
    }
    return false;
  }

  const bool continuing =
      track->reanchor_pending &&
      now_ms - track->reanchor_last_ms <=
          this->policy_.reanchor_timeout_ms &&
      distance_between(track->reanchor_x_mm, track->reanchor_y_mm,
                       measurement.x_mm, measurement.y_mm) <=
          this->policy_.reanchor_consistency_gate_mm;
  if (!continuing) {
    track->reanchor_pending = true;
    track->reanchor_hits = 1;
    track->reanchor_x_mm = measurement.x_mm;
    track->reanchor_y_mm = measurement.y_mm;
  } else {
    if (track->reanchor_hits < 255)
      track->reanchor_hits++;
    track->reanchor_x_mm =
        (track->reanchor_x_mm + measurement.x_mm) * 0.5f;
    track->reanchor_y_mm =
        (track->reanchor_y_mm + measurement.y_mm) * 0.5f;
  }
  track->reanchor_last_ms = now_ms;
  if (track->reanchor_hits < this->policy_.reanchor_hits_required)
    return false;

  track->value = measurement;
  track->state = SpatialShadowTrackState::CONFIRMED;
  track->last_seen_ms = now_ms;
  track->last_update_ms = now_ms;
  if (track->hit_count < std::numeric_limits<uint16_t>::max())
    track->hit_count++;
  remember_measurement_identity_(track, track->value);
  track->reanchor_pending = false;
  track->reanchor_hits = 0;
  track->reanchor_last_ms = 0;
  return true;
}

void SpatialShadowFusion::remember_measurement_identity_(
    TrackSlot *track, const Measurement &measurement) {
  if (track == nullptr)
    return;
  for (std::size_t measurement_index = 0;
       measurement_index < measurement.source_count; ++measurement_index) {
    const uint64_t participant =
        measurement.participant_tokens[measurement_index];
    const uint32_t local_track_id =
        measurement.local_track_ids[measurement_index];
    if (participant == 0 || local_track_id == 0)
      continue;

    bool stored = false;
    for (std::size_t identity_index = 0;
         identity_index < track->identity_count; ++identity_index) {
      if (track->identity_participant_tokens[identity_index] != participant)
        continue;
      track->identity_local_track_ids[identity_index] = local_track_id;
      stored = true;
      break;
    }
    if (stored ||
        track->identity_count >= track->identity_participant_tokens.size())
      continue;
    const std::size_t index = track->identity_count++;
    track->identity_participant_tokens[index] = participant;
    track->identity_local_track_ids[index] = local_track_id;
  }
}

uint8_t SpatialShadowFusion::confirmation_hits_for_(
    const Measurement &measurement, bool established_track_exists) const {
  if (measurement.source_count >= 2)
    return std::max<uint8_t>(1, this->policy_.confirmation_hits);
  const uint8_t required =
      established_track_exists
          ? this->policy_.additional_single_source_confirmation_hits
          : this->policy_.single_source_confirmation_hits;
  return std::max<uint8_t>(1, required);
}

const SpatialShadowFusion::TrackSlot *SpatialShadowFusion::find_track_by_id_(
    uint32_t fused_track_id) const {
  if (fused_track_id == 0)
    return nullptr;
  for (const auto &track : this->tracks_) {
    if (track.occupied && track.fused_track_id == fused_track_id)
      return &track;
  }
  return nullptr;
}

const SpatialShadowFusion::TrackSlot *
SpatialShadowFusion::nearest_reference_track_(
    const Measurement &measurement) const {
  const TrackSlot *best = nullptr;
  float best_distance = 0.0f;
  for (const auto &track : this->tracks_) {
    if (!track.occupied)
      continue;
    const float distance =
        distance_between(track.value.x_mm, track.value.y_mm,
                         measurement.x_mm, measurement.y_mm);
    if (best == nullptr || distance < best_distance) {
      best = &track;
      best_distance = distance;
    }
  }
  return best;
}

void SpatialShadowFusion::update_secondary_evidence_(TrackSlot *track) {
  if (track == nullptr || !track->secondary_guarded ||
      track->secondary_independent)
    return;
  const TrackSlot *parent =
      this->find_track_by_id_(track->secondary_parent_track_id);
  if (parent == nullptr) {
    track->secondary_guarded = false;
    return;
  }

  const float relative_x = track->value.x_mm - parent->value.x_mm;
  const float relative_y = track->value.y_mm - parent->value.y_mm;
  if (!track->secondary_relative_initialized) {
    track->secondary_relative_initialized = true;
    track->secondary_initial_relative_x_mm = relative_x;
    track->secondary_initial_relative_y_mm = relative_y;
    return;
  }
  const float displacement =
      distance_between(relative_x, relative_y,
                       track->secondary_initial_relative_x_mm,
                       track->secondary_initial_relative_y_mm);
  if (displacement < this->policy_.secondary_independent_displacement_mm) {
    track->secondary_independent_hits = 0;
    return;
  }
  if (track->secondary_independent_hits < 255)
    track->secondary_independent_hits++;
  if (track->secondary_independent_hits >=
      this->policy_.secondary_independent_hits_required)
    track->secondary_independent = true;
}

bool SpatialShadowFusion::should_confirm_track_(const TrackSlot &track,
                                                uint32_t now_ms) const {
  if (track.hit_count < track.confirmation_hits_required)
    return false;
  if (!track.secondary_guarded || track.secondary_independent)
    return true;
  return now_ms - track.first_seen_ms >=
         this->policy_.additional_single_source_guard_ms;
}

}  // namespace radar_api_server
}  // namespace esphome
