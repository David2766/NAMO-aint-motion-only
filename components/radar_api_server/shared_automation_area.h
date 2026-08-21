#pragma once

#include "device_config_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct SharedAutomationArea {
  static constexpr std::size_t MAX_ID_LENGTH = 40;
  static constexpr std::size_t MAX_NAME_LENGTH = 48;
  static constexpr std::size_t MAX_POINTS = 8;

  bool occupied{false};
  char area_id[MAX_ID_LENGTH + 1]{};
  char name[MAX_NAME_LENGTH + 1]{};
  uint8_t point_count{0};
  std::array<float, MAX_POINTS> x_mm{};
  std::array<float, MAX_POINTS> y_mm{};

  bool contains(float x, float y) const;
};

class SharedAutomationAreaCache {
 public:
  static constexpr std::size_t MAX_AREAS = 6;

  bool update(const char *json, std::size_t size,
              const char *expected_floorplan_id, const char *space_id,
              uint32_t config_hash, const FloorplanRadarPose &pose);
  void clear();

  bool matches(const char *floorplan_id, const char *space_id,
               uint32_t config_hash, uint32_t pose_version) const;
  bool configuration_valid() const { return this->configuration_valid_; }
  uint32_t revision() const { return this->revision_; }
  uint32_t config_hash() const { return this->config_hash_; }
  uint32_t pose_version() const { return this->pose_version_; }
  uint8_t size() const { return this->area_count_; }
  const char *floorplan_id() const { return this->floorplan_id_; }
  const char *space_id() const { return this->space_id_; }
  const SharedAutomationArea &area(std::size_t index) const;

 private:
  bool attempted_{false};
  bool configuration_valid_{false};
  char floorplan_id_[SharedAutomationArea::MAX_ID_LENGTH + 1]{};
  char space_id_[SharedAutomationArea::MAX_ID_LENGTH + 1]{};
  uint32_t revision_{0};
  uint32_t config_hash_{0};
  uint32_t pose_version_{0};
  uint8_t area_count_{0};
  std::array<SharedAutomationArea, MAX_AREAS> areas_{};
};

static_assert(std::is_trivially_copyable<SharedAutomationAreaCache>::value,
              "shared automation area cache must not own heap-backed state");
static_assert(sizeof(SharedAutomationAreaCache) <= 1536,
              "shared automation area cache exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
