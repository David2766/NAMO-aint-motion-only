#pragma once

#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct SpatialPointMm {
  float x{0.0f};
  float y{0.0f};
};

// Fixed-size, persisted local-sensor to Floorplan transform. The correction is
// proposed by calibration but only this approved effective transform is stored.
struct SpatialTransform2D {
  int32_t origin_x_mm{0};
  int32_t origin_y_mm{0};
  int16_t rotation_cdeg{0};
  // These bounded values share one word so nineteen participant profiles do
  // not add another four-byte-aligned block per record.
  uint16_t residual_mm : 10;
  uint16_t sample_count : 2;
  uint16_t valid : 1;
  uint16_t reserved : 3;
};

bool valid_spatial_transform(const SpatialTransform2D &transform);
SpatialPointMm apply_spatial_transform(const SpatialTransform2D &transform,
                                       float local_x_mm, float local_y_mm);
SpatialPointMm apply_inverse_spatial_transform(
    const SpatialTransform2D &transform, float world_x_mm, float world_y_mm);
SpatialPointMm apply_inverse_spatial_vector(
    const SpatialTransform2D &transform, float world_x_mm_s,
    float world_y_mm_s);

static_assert(std::is_trivially_copyable<SpatialTransform2D>::value,
              "spatial transforms must not own heap-backed state");
static_assert(sizeof(SpatialTransform2D) <= 12,
              "spatial transform exceeded the embedded record budget");

}  // namespace radar_api_server
}  // namespace esphome
