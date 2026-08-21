#pragma once

#include "spatial_shadow_runtime.h"
#include "spatial_transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class SpatialProductionFusionState : uint8_t {
  DISABLED = 0,
  DEGRADED,
  ACTIVE,
};

const char *spatial_production_fusion_state_to_string(
    SpatialProductionFusionState state);

struct SpatialProductionTarget {
  bool valid{false};
  uint32_t fused_track_id{0};
  SpatialShadowTrackState state{SpatialShadowTrackState::EMPTY};
  uint8_t source_count{0};
  uint8_t confidence{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
};

struct SpatialProductionFusionOutput {
  static constexpr std::size_t MAX_TARGETS = 3;

  bool build_enabled{false};
  bool authoritative{false};
  SpatialProductionFusionState state{
      SpatialProductionFusionState::DISABLED};
  uint32_t source_sequence{0};
  uint8_t target_count{0};
  std::array<SpatialProductionTarget, MAX_TARGETS> targets{};
};

class SpatialProductionFusion {
 public:
  void update(bool build_enabled,
              const SpatialShadowRuntimeDiagnostics *runtime,
              const SpatialShadowFusionOutput *fusion,
              const SpatialTransform2D &local_transform);
  void clear();

  const SpatialProductionFusionOutput &output() const {
    return this->output_;
  }

 private:
  SpatialProductionFusionOutput output_{};
};

static_assert(std::is_trivially_copyable<SpatialProductionFusion>::value,
              "production fusion adapter must not own heap-backed state");
static_assert(sizeof(SpatialProductionFusion) <= 256,
              "production fusion adapter exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
