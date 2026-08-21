#include "spatial_production_fusion.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "spatial_production_fusion_tests: %s\n", message);
  std::exit(1);
}

bool near(float actual, float expected) {
  return std::fabs(actual - expected) < 0.1f;
}

SpatialTransform2D local_transform() {
  SpatialTransform2D value;
  value.origin_x_mm = 1000;
  value.origin_y_mm = 2000;
  value.rotation_cdeg = 9000;
  value.residual_mm = 0;
  value.sample_count = 1;
  value.valid = true;
  return value;
}

void production_adapter_is_explicit_and_fails_closed() {
  SpatialProductionFusion adapter;
  SpatialShadowRuntimeDiagnostics runtime;
  SpatialShadowFusionOutput fusion;
  adapter.update(false, &runtime, &fusion, local_transform());
  require(!adapter.output().build_enabled &&
              adapter.output().state ==
                  SpatialProductionFusionState::DISABLED,
          "production authority must default off");

  adapter.update(true, &runtime, &fusion, local_transform());
  require(adapter.output().build_enabled &&
              !adapter.output().authoritative &&
              adapter.output().state ==
                  SpatialProductionFusionState::DEGRADED,
          "an inactive runtime must preserve the local production path");
}

void production_adapter_publishes_only_stable_local_targets() {
  SpatialProductionFusion adapter;
  SpatialShadowRuntimeDiagnostics runtime;
  runtime.state = SpatialShadowRuntimeState::ACTIVE;
  SpatialShadowFusionOutput fusion;
  fusion.sequence = 7;
  fusion.track_count = 3;
  fusion.tracks[0].valid = true;
  fusion.tracks[0].state = SpatialShadowTrackState::TENTATIVE;
  fusion.tracks[1].valid = true;
  fusion.tracks[1].state = SpatialShadowTrackState::CONFIRMED;
  fusion.tracks[1].fused_track_id = 42;
  fusion.tracks[1].source_count = 2;
  fusion.tracks[1].confidence = 91;
  fusion.tracks[1].x_mm = 1300.0f;
  fusion.tracks[1].y_mm = 2100.0f;
  fusion.tracks[1].vx_mm_s = 20.0f;
  fusion.tracks[1].vy_mm_s = 10.0f;
  fusion.tracks[2].valid = true;
  fusion.tracks[2].state = SpatialShadowTrackState::COASTING;
  fusion.tracks[2].fused_track_id = 43;
  fusion.tracks[2].x_mm = 1400.0f;
  fusion.tracks[2].y_mm = 2200.0f;

  adapter.update(true, &runtime, &fusion, local_transform());
  const auto &output = adapter.output();
  require(output.authoritative &&
              output.state == SpatialProductionFusionState::ACTIVE &&
              output.source_sequence == 7 && output.target_count == 2,
          "active production output must contain confirmed and coasting tracks");
  require(output.targets[0].fused_track_id == 42 &&
              near(output.targets[0].x_mm, 100.0f) &&
              near(output.targets[0].y_mm, 300.0f) &&
              near(output.targets[0].vx_mm_s, 10.0f) &&
              near(output.targets[0].vy_mm_s, 20.0f),
          "world coordinates must be converted back to the leader radar frame");
  require(output.targets[1].fused_track_id == 43 &&
              !output.targets[2].valid,
          "production output must remain bounded to three compact targets");
}

}  // namespace

void run_spatial_production_fusion_tests() {
  production_adapter_is_explicit_and_fails_closed();
  production_adapter_publishes_only_stable_local_targets();
}
