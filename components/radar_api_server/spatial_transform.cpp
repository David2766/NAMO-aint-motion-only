#include "spatial_transform.h"

#include <cmath>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr int32_t MAX_ORIGIN_MM = 1000000;
constexpr uint16_t MAX_ACCEPTED_RESIDUAL_MM = 1000;

}  // namespace

bool valid_spatial_transform(const SpatialTransform2D &transform) {
  return transform.valid && transform.origin_x_mm >= -MAX_ORIGIN_MM &&
         transform.origin_x_mm <= MAX_ORIGIN_MM &&
         transform.origin_y_mm >= -MAX_ORIGIN_MM &&
         transform.origin_y_mm <= MAX_ORIGIN_MM &&
         transform.rotation_cdeg >= -18000 &&
         transform.rotation_cdeg <= 18000 &&
         transform.residual_mm <= MAX_ACCEPTED_RESIDUAL_MM &&
         transform.sample_count >= 1 && transform.sample_count <= 2;
}

SpatialPointMm apply_spatial_transform(const SpatialTransform2D &transform,
                                       float local_x_mm, float local_y_mm) {
  if (!valid_spatial_transform(transform))
    return {};
  const float angle = static_cast<float>(transform.rotation_cdeg) *
                      0.01f * 3.14159265358979323846f / 180.0f;
  const float cosine = std::cos(angle);
  const float sine = std::sin(angle);
  return {
      static_cast<float>(transform.origin_x_mm) + local_x_mm * cosine +
          local_y_mm * sine,
      static_cast<float>(transform.origin_y_mm) + local_x_mm * sine -
          local_y_mm * cosine,
  };
}

SpatialPointMm apply_inverse_spatial_transform(
    const SpatialTransform2D &transform, float world_x_mm, float world_y_mm) {
  if (!valid_spatial_transform(transform))
    return {};
  return apply_inverse_spatial_vector(
      transform, world_x_mm - static_cast<float>(transform.origin_x_mm),
      world_y_mm - static_cast<float>(transform.origin_y_mm));
}

SpatialPointMm apply_inverse_spatial_vector(
    const SpatialTransform2D &transform, float world_x_mm_s,
    float world_y_mm_s) {
  if (!valid_spatial_transform(transform))
    return {};
  const float angle = static_cast<float>(transform.rotation_cdeg) *
                      0.01f * 3.14159265358979323846f / 180.0f;
  const float cosine = std::cos(angle);
  const float sine = std::sin(angle);
  return {
      world_x_mm_s * cosine + world_y_mm_s * sine,
      world_x_mm_s * sine - world_y_mm_s * cosine,
  };
}

}  // namespace radar_api_server
}  // namespace esphome
