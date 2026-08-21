#include "spatial_production_fusion.h"

namespace esphome {
namespace radar_api_server {

const char *spatial_production_fusion_state_to_string(
    SpatialProductionFusionState state) {
  switch (state) {
    case SpatialProductionFusionState::DISABLED:
      return "disabled";
    case SpatialProductionFusionState::DEGRADED:
      return "degraded";
    case SpatialProductionFusionState::ACTIVE:
      return "active";
  }
  return "disabled";
}

void SpatialProductionFusion::update(
    bool build_enabled, const SpatialShadowRuntimeDiagnostics *runtime,
    const SpatialShadowFusionOutput *fusion,
    const SpatialTransform2D &local_transform) {
  this->clear();
  this->output_.build_enabled = build_enabled;
  if (!build_enabled)
    return;
  this->output_.state = SpatialProductionFusionState::DEGRADED;
  if (runtime == nullptr || fusion == nullptr ||
      runtime->state != SpatialShadowRuntimeState::ACTIVE ||
      !valid_spatial_transform(local_transform))
    return;

  this->output_.state = SpatialProductionFusionState::ACTIVE;
  this->output_.authoritative = true;
  this->output_.source_sequence = fusion->sequence;
  for (const auto &track : fusion->tracks) {
    if (!track.valid ||
        (track.state != SpatialShadowTrackState::CONFIRMED &&
         track.state != SpatialShadowTrackState::COASTING) ||
        this->output_.target_count >= this->output_.targets.size())
      continue;
    auto &target =
        this->output_.targets[this->output_.target_count++];
    const auto point = apply_inverse_spatial_transform(
        local_transform, track.x_mm, track.y_mm);
    const auto velocity = apply_inverse_spatial_vector(
        local_transform, track.vx_mm_s, track.vy_mm_s);
    target.valid = true;
    target.fused_track_id = track.fused_track_id;
    target.state = track.state;
    target.source_count = track.source_count;
    target.confidence = track.confidence;
    target.x_mm = point.x;
    target.y_mm = point.y;
    target.vx_mm_s = velocity.x;
    target.vy_mm_s = velocity.y;
  }
}

void SpatialProductionFusion::clear() {
  this->output_ = {};
}

}  // namespace radar_api_server
}  // namespace esphome
