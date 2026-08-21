#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "spatial_transform.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace esphome {
namespace radar_api_server {

static constexpr int MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS = 18;
static constexpr int MAX_ZONE_POINTS = 8;
static constexpr int MAX_SOFTWARE_ZONES = 6;
static constexpr int MAX_CALIBRATION_ZONES = 4;

enum class CachedZoneType : uint8_t {
  INVALID = 0,
  DETECTION,
  FILTER,
  REDUCED,
  DISABLED,
  EXIT,
};

struct CachedZone {
  bool valid{false};
  CachedZoneType type{CachedZoneType::INVALID};
  uint8_t point_count{0};
  float x[MAX_ZONE_POINTS]{};
  float y[MAX_ZONE_POINTS]{};

  bool contains(float target_x, float target_y) const;
};

struct FloorplanRoomContext {
  bool configured{false};
  std::string id{};
  std::string name{};
  int boundary_point_count{0};
  float boundary_x[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS]{};
  float boundary_y[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS]{};
};

struct FloorplanRadarPose {
  bool valid{false};
  float origin_x_px{0.0f};
  float origin_y_px{0.0f};
  float rotation_deg{0.0f};
  float scale_factor{1.0f};
  float mm_per_px_x{0.0f};
  float mm_per_px_y{0.0f};
  uint32_t pose_version{0};

  SpatialTransform2D spatial_transform() const;
};

class DeviceConfigCache {
 public:
  DeviceConfigCache();

  class ReadGuard {
   public:
    explicit ReadGuard(const DeviceConfigCache *cache);
    ~ReadGuard();
    ReadGuard(const ReadGuard &) = delete;
    ReadGuard &operator=(const ReadGuard &) = delete;
    ReadGuard(ReadGuard &&other) noexcept;
    ReadGuard &operator=(ReadGuard &&) = delete;

    const DeviceConfigCache *operator->() const { return this->cache_; }
    const DeviceConfigCache &get() const { return *this->cache_; }

   private:
    const DeviceConfigCache *cache_{nullptr};
  };

  ReadGuard read() const { return ReadGuard(this); }
  void update(std::string json);
  void clear();
  bool hydrate_floorplan_scale_from_document(const std::string &json);

  bool has_config() const { return this->has_config_; }
  const std::string &raw_config() const { return this->raw_config_; }
  const CachedZone &software_zone(int zone_index) const;
  const CachedZone &calibration_zone(int zone_index) const;
  const std::string &software_zone_name(int zone_index) const;
  bool tracker_assist_presence_enabled() const { return this->tracker_assist_presence_enabled_; }
  bool legacy_presence_fallback_enabled() const { return this->legacy_presence_fallback_enabled_; }
  const FloorplanRoomContext &floorplan_room() const { return this->floorplan_room_; }
  uint32_t floorplan_pose_version() const { return this->floorplan_pose_version_; }
  bool floorplan_scale_hydrated() const {
    return this->floorplan_scale_hydrated_;
  }
  const FloorplanRadarPose &floorplan_radar_pose() const {
    return this->floorplan_radar_pose_;
  }
  bool point_in_floorplan_room(float x, float y) const;
  float floorplan_room_signed_distance(float x, float y) const;

 private:
  bool has_config_{false};
  bool tracker_assist_presence_enabled_{true};
  bool legacy_presence_fallback_enabled_{false};
  std::string raw_config_;
  std::array<CachedZone, MAX_SOFTWARE_ZONES> software_zones_{};
  std::array<CachedZone, MAX_CALIBRATION_ZONES> calibration_zones_{};
  std::array<std::string, MAX_SOFTWARE_ZONES> software_zone_names_{};
  FloorplanRoomContext floorplan_room_{};
  FloorplanRadarPose floorplan_radar_pose_{};
  uint32_t floorplan_pose_version_{0};
  bool floorplan_scale_hydrated_{false};
#ifdef USE_ESP32
  mutable StaticSemaphore_t mutex_storage_{};
  mutable SemaphoreHandle_t mutex_{nullptr};
#endif

  static const std::string EMPTY_;
  static const CachedZone EMPTY_ZONE_;

  void refresh_zone_cache_();
  void refresh_floorplan_room_cache_();
  static CachedZone parse_zone_(const std::string &json);
  static std::string extract_object_by_id_(const std::string &json, const std::string &id);
  static std::string extract_object_by_key_(const std::string &json, const std::string &key);
  static bool extract_bool_(const std::string &json, const std::string &key, bool fallback);
  static uint32_t extract_uint32_(const std::string &json, const std::string &key);
  static bool extract_float_(const std::string &json, const std::string &key,
                             float *value);
  static std::string extract_string_(const std::string &json, const std::string &key);
  static int extract_boundary_points_(const std::string &json, float xs[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS],
                                      float ys[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS]);
  void lock_() const;
  void unlock_() const;
};

}  // namespace radar_api_server
}  // namespace esphome
