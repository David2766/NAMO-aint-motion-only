#include "device_config_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace esphome {
namespace radar_api_server {

const std::string DeviceConfigCache::EMPTY_{};
const CachedZone DeviceConfigCache::EMPTY_ZONE_{};

DeviceConfigCache::DeviceConfigCache() {
#ifdef USE_ESP32
  this->mutex_ = xSemaphoreCreateMutexStatic(&this->mutex_storage_);
#endif
}

DeviceConfigCache::ReadGuard::ReadGuard(const DeviceConfigCache *cache)
    : cache_(cache) {
  if (this->cache_ != nullptr)
    this->cache_->lock_();
}

DeviceConfigCache::ReadGuard::~ReadGuard() {
  if (this->cache_ != nullptr)
    this->cache_->unlock_();
}

DeviceConfigCache::ReadGuard::ReadGuard(ReadGuard &&other) noexcept
    : cache_(other.cache_) {
  other.cache_ = nullptr;
}

void DeviceConfigCache::lock_() const {
#ifdef USE_ESP32
  if (this->mutex_ != nullptr)
    xSemaphoreTake(this->mutex_, portMAX_DELAY);
#endif
}

void DeviceConfigCache::unlock_() const {
#ifdef USE_ESP32
  if (this->mutex_ != nullptr)
    xSemaphoreGive(this->mutex_);
#endif
}

namespace {

float distance_to_segment_(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax;
  const float vy = by - ay;
  const float wx = px - ax;
  const float wy = py - ay;
  const float len2 = vx * vx + vy * vy;
  if (len2 <= 0.0001f)
    return std::sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));

  float t = (wx * vx + wy * vy) / len2;
  if (t < 0.0f)
    t = 0.0f;
  else if (t > 1.0f)
    t = 1.0f;

  const float cx = ax + t * vx;
  const float cy = ay + t * vy;
  return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

float normalize_degrees_(float degrees) {
  while (degrees > 180.0f)
    degrees -= 360.0f;
  while (degrees < -180.0f)
    degrees += 360.0f;
  return degrees;
}

}  // namespace

SpatialTransform2D FloorplanRadarPose::spatial_transform() const {
  SpatialTransform2D transform;
  if (!this->valid)
    return transform;
  transform.origin_x_mm =
      static_cast<int32_t>(std::lround(this->origin_x_px * this->mm_per_px_x));
  transform.origin_y_mm =
      static_cast<int32_t>(std::lround(this->origin_y_px * this->mm_per_px_y));
  transform.rotation_cdeg = static_cast<int16_t>(
      std::lround(normalize_degrees_(this->rotation_deg) * 100.0f));
  transform.residual_mm = 0;
  transform.sample_count = 1;
  transform.valid = true;
  return transform;
}

bool CachedZone::contains(float target_x, float target_y) const {
  if (!this->valid || this->point_count < 3)
    return false;

  bool inside = false;
  for (int i = 0, j = this->point_count - 1; i < this->point_count; j = i++) {
    const bool intersects =
        ((this->y[i] > target_y) != (this->y[j] > target_y)) &&
        (target_x < (this->x[j] - this->x[i]) * (target_y - this->y[i]) /
                            ((this->y[j] - this->y[i]) == 0 ? 0.0001f : (this->y[j] - this->y[i])) +
                        this->x[i]);
    if (intersects)
      inside = !inside;
  }
  return inside;
}

void DeviceConfigCache::update(std::string json) {
  const auto guard = this->read();
  this->has_config_ = !json.empty();
  this->raw_config_ = std::move(json);
  this->refresh_zone_cache_();
}

void DeviceConfigCache::clear() {
  const auto guard = this->read();
  this->raw_config_.clear();
  this->has_config_ = false;
  this->tracker_assist_presence_enabled_ = true;
  this->legacy_presence_fallback_enabled_ = false;
  for (auto &zone : this->software_zones_)
    zone = {};
  for (auto &zone : this->calibration_zones_)
    zone = {};
  for (auto &name : this->software_zone_names_)
    name.clear();
  this->floorplan_room_ = {};
  this->floorplan_radar_pose_ = {};
  this->floorplan_pose_version_ = 0;
  this->floorplan_scale_hydrated_ = false;
}

const CachedZone &DeviceConfigCache::software_zone(int zone_index) const {
  if (zone_index < 1 || zone_index > static_cast<int>(this->software_zones_.size()))
    return EMPTY_ZONE_;
  return this->software_zones_[zone_index - 1];
}

const CachedZone &DeviceConfigCache::calibration_zone(int zone_index) const {
  if (zone_index < 1 || zone_index > static_cast<int>(this->calibration_zones_.size()))
    return EMPTY_ZONE_;
  return this->calibration_zones_[zone_index - 1];
}

const std::string &DeviceConfigCache::software_zone_name(int zone_index) const {
  if (zone_index < 1 || zone_index > static_cast<int>(this->software_zone_names_.size()))
    return EMPTY_;
  return this->software_zone_names_[zone_index - 1];
}

void DeviceConfigCache::refresh_zone_cache_() {
  this->legacy_presence_fallback_enabled_ = extract_bool_(this->raw_config_, "legacyPresenceFallback", false);
  this->tracker_assist_presence_enabled_ = !this->legacy_presence_fallback_enabled_;
  for (int index = 1; index <= static_cast<int>(this->software_zones_.size()); index++) {
    const std::string config = extract_object_by_id_(this->raw_config_, "zone_" + std::to_string(index));
    this->software_zones_[index - 1] = parse_zone_(config);
    this->software_zone_names_[index - 1] = extract_string_(config, "name");
  }
  for (int index = 1; index <= static_cast<int>(this->calibration_zones_.size()); index++) {
    const std::string config =
        extract_object_by_id_(this->raw_config_, "calibration_" + std::to_string(index));
    this->calibration_zones_[index - 1] = parse_zone_(config);
  }
  this->refresh_floorplan_room_cache_();
}

CachedZone DeviceConfigCache::parse_zone_(const std::string &json) {
  CachedZone zone;
  if (json.empty() || json == "__EMPTY__")
    return zone;

  const std::string type = extract_string_(json, "type");
  if (type == "detection")
    zone.type = CachedZoneType::DETECTION;
  else if (type == "filter")
    zone.type = CachedZoneType::FILTER;
  else if (type == "reduced")
    zone.type = CachedZoneType::REDUCED;
  else if (type == "disabled")
    zone.type = CachedZoneType::DISABLED;
  else if (type == "exit")
    zone.type = CachedZoneType::EXIT;
  else
    return zone;

  const size_t points_pos = json.find("\"points\"");
  if (points_pos == std::string::npos)
    return zone;
  const size_t end = json.size();
  size_t pos = json.find('[', points_pos);
  if (pos == std::string::npos || pos >= end)
    return zone;

  auto read_number = [&](size_t &cursor, float *value) -> bool {
    while (cursor < end && !(json[cursor] == '-' || json[cursor] == '+' ||
                             (json[cursor] >= '0' && json[cursor] <= '9')))
      cursor++;
    if (cursor >= end)
      return false;
    char *number_end = nullptr;
    *value = std::strtof(json.c_str() + cursor, &number_end);
    if (number_end == json.c_str() + cursor || !std::isfinite(*value))
      return false;
    cursor = static_cast<size_t>(number_end - json.c_str());
    return true;
  };

  while (pos < end && zone.point_count < MAX_ZONE_POINTS) {
    float x = 0.0f;
    float y = 0.0f;
    if (!read_number(pos, &x) || !read_number(pos, &y))
      break;
    zone.x[zone.point_count] = std::max(-4860.0f, std::min(4860.0f, x));
    zone.y[zone.point_count] = std::max(0.0f, std::min(7560.0f, y));
    zone.point_count++;

    const size_t next = json.find('[', pos);
    const size_t points_close = json.find("]]", pos);
    if (next == std::string::npos || (points_close != std::string::npos && points_close < next) || next >= end)
      break;
    pos = next;
  }

  zone.valid = zone.point_count >= 3;
  if (!zone.valid)
    zone = {};
  return zone;
}

void DeviceConfigCache::refresh_floorplan_room_cache_() {
  this->floorplan_room_ = {};
  this->floorplan_radar_pose_ = {};
  this->floorplan_pose_version_ = 0;
  this->floorplan_scale_hydrated_ = false;
  const std::string floorplan = extract_object_by_key_(this->raw_config_, "floorplan");
  if (floorplan.empty())
    return;
  const std::string radar = extract_object_by_key_(floorplan, "radar");
  if (!radar.empty()) {
    this->floorplan_pose_version_ = extract_uint32_(radar, "poseVersion");
    const bool legacy_placement =
        radar.find("\"poseVersion\"") == std::string::npos &&
        radar.find("\"originX\"") != std::string::npos &&
        radar.find("\"originY\"") != std::string::npos &&
        radar.find("\"rotation\"") != std::string::npos &&
        radar.find("\"scale\"") != std::string::npos;
    if (this->floorplan_pose_version_ == 0 && legacy_placement)
      this->floorplan_pose_version_ = 1;

    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float rotation = 0.0f;
    float scale_factor = 0.0f;
    float mm_per_px_x = 0.0f;
    float mm_per_px_y = 0.0f;
    const bool has_axis_scale =
        extract_float_(floorplan, "scaleMmPerPxX", &mm_per_px_x) &&
        extract_float_(floorplan, "scaleMmPerPxY", &mm_per_px_y);
    if (!has_axis_scale) {
      float legacy_mm_per_px = 0.0f;
      if (extract_float_(floorplan, "scaleMmPerPx", &legacy_mm_per_px)) {
        mm_per_px_x = legacy_mm_per_px;
        mm_per_px_y = legacy_mm_per_px;
      }
    }
    if (this->floorplan_pose_version_ != 0 &&
        extract_float_(radar, "originX", &origin_x) &&
        extract_float_(radar, "originY", &origin_y) &&
        extract_float_(radar, "rotation", &rotation) &&
        extract_float_(radar, "scale", &scale_factor) &&
        std::isfinite(origin_x) && std::isfinite(origin_y) &&
        std::isfinite(rotation) && std::isfinite(scale_factor) &&
        std::isfinite(mm_per_px_x) &&
        std::isfinite(mm_per_px_y) &&
        std::fabs(origin_x) <= 100000.0f &&
        std::fabs(origin_y) <= 100000.0f && scale_factor > 0.0f &&
        scale_factor <= 10.0f && mm_per_px_x > 0.0f &&
        mm_per_px_x <= 10000.0f && mm_per_px_y > 0.0f &&
        mm_per_px_y <= 10000.0f) {
      this->floorplan_radar_pose_.valid = true;
      this->floorplan_radar_pose_.origin_x_px = origin_x;
      this->floorplan_radar_pose_.origin_y_px = origin_y;
      this->floorplan_radar_pose_.rotation_deg = rotation;
      this->floorplan_radar_pose_.scale_factor = scale_factor;
      this->floorplan_radar_pose_.mm_per_px_x = mm_per_px_x;
      this->floorplan_radar_pose_.mm_per_px_y = mm_per_px_y;
      this->floorplan_radar_pose_.pose_version =
          this->floorplan_pose_version_;
    }
  }
  const std::string room = extract_object_by_key_(floorplan, "room");
  if (room.empty())
    return;

  this->floorplan_room_.id = extract_string_(room, "id");
  this->floorplan_room_.name = extract_string_(room, "name");
  this->floorplan_room_.boundary_point_count =
      extract_boundary_points_(room, this->floorplan_room_.boundary_x, this->floorplan_room_.boundary_y);
  this->floorplan_room_.configured =
      !this->floorplan_room_.id.empty() || !this->floorplan_room_.name.empty() ||
      this->floorplan_room_.boundary_point_count >= 3;
}

bool DeviceConfigCache::hydrate_floorplan_scale_from_document(
    const std::string &json) {
  const auto guard = this->read();
  if (this->floorplan_radar_pose_.valid ||
      this->floorplan_pose_version_ == 0 || json.empty())
    return false;

  const std::string config_floorplan =
      extract_object_by_key_(this->raw_config_, "floorplan");
  const std::string radar =
      extract_object_by_key_(config_floorplan, "radar");
  const std::string scale = extract_object_by_key_(json, "scale");
  if (radar.empty() || scale.empty())
    return false;

  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float rotation = 0.0f;
  float scale_factor = 0.0f;
  float mm_per_px_x = 0.0f;
  float mm_per_px_y = 0.0f;
  if (!extract_float_(radar, "originX", &origin_x) ||
      !extract_float_(radar, "originY", &origin_y) ||
      !extract_float_(radar, "rotation", &rotation) ||
      !extract_float_(radar, "scale", &scale_factor) ||
      !extract_float_(scale, "mmPerPxX", &mm_per_px_x) ||
      !extract_float_(scale, "mmPerPxY", &mm_per_px_y) ||
      !std::isfinite(origin_x) || !std::isfinite(origin_y) ||
      !std::isfinite(rotation) || !std::isfinite(scale_factor) ||
      !std::isfinite(mm_per_px_x) ||
      !std::isfinite(mm_per_px_y) ||
      std::fabs(origin_x) > 100000.0f ||
      std::fabs(origin_y) > 100000.0f ||
      scale_factor <= 0.0f || scale_factor > 10.0f ||
      mm_per_px_x <= 0.0f || mm_per_px_x > 10000.0f ||
      mm_per_px_y <= 0.0f || mm_per_px_y > 10000.0f)
    return false;

  this->floorplan_radar_pose_.valid = true;
  this->floorplan_radar_pose_.origin_x_px = origin_x;
  this->floorplan_radar_pose_.origin_y_px = origin_y;
  this->floorplan_radar_pose_.rotation_deg = rotation;
  this->floorplan_radar_pose_.scale_factor = scale_factor;
  this->floorplan_radar_pose_.mm_per_px_x = mm_per_px_x;
  this->floorplan_radar_pose_.mm_per_px_y = mm_per_px_y;
  this->floorplan_radar_pose_.pose_version =
      this->floorplan_pose_version_;
  this->floorplan_scale_hydrated_ = true;
  return true;
}

bool DeviceConfigCache::point_in_floorplan_room(float x, float y) const {
  const auto &room = this->floorplan_room_;
  if (room.boundary_point_count < 3)
    return false;

  bool inside = false;
  for (int i = 0, j = room.boundary_point_count - 1; i < room.boundary_point_count; j = i++) {
    const bool intersects = ((room.boundary_y[i] > y) != (room.boundary_y[j] > y)) &&
                            (x < (room.boundary_x[j] - room.boundary_x[i]) * (y - room.boundary_y[i]) /
                                      ((room.boundary_y[j] - room.boundary_y[i]) == 0 ? 0.0001f
                                                                                      : (room.boundary_y[j] - room.boundary_y[i])) +
                                      room.boundary_x[i]);
    if (intersects)
      inside = !inside;
  }
  return inside;
}

float DeviceConfigCache::floorplan_room_signed_distance(float x, float y) const {
  const auto &room = this->floorplan_room_;
  if (room.boundary_point_count < 3)
    return 0.0f;

  float min_distance = std::numeric_limits<float>::max();
  for (int i = 0; i < room.boundary_point_count; i++) {
    const int next = (i + 1) % room.boundary_point_count;
    const float distance = distance_to_segment_(x, y, room.boundary_x[i], room.boundary_y[i],
                                                room.boundary_x[next], room.boundary_y[next]);
    if (distance < min_distance)
      min_distance = distance;
  }

  if (min_distance == std::numeric_limits<float>::max())
    return 0.0f;
  return this->point_in_floorplan_room(x, y) ? min_distance : -min_distance;
}

bool DeviceConfigCache::extract_bool_(const std::string &json, const std::string &key, bool fallback) {
  const std::string needle = "\"" + key + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos)
    return fallback;

  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos)
    return fallback;

  size_t value_pos = colon_pos + 1;
  while (value_pos < json.size() && std::isspace(static_cast<unsigned char>(json[value_pos])))
    value_pos++;

  if (json.compare(value_pos, 4, "true") == 0)
    return true;
  if (json.compare(value_pos, 5, "false") == 0)
    return false;
  return fallback;
}

uint32_t DeviceConfigCache::extract_uint32_(const std::string &json,
                                            const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos)
    return 0;
  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos)
    return 0;

  size_t cursor = colon_pos + 1;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n'))
    ++cursor;
  if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9')
    return 0;

  uint32_t value = 0;
  while (cursor < json.size() && json[cursor] >= '0' &&
         json[cursor] <= '9') {
    const uint32_t digit = static_cast<uint32_t>(json[cursor] - '0');
    if (value > (UINT32_MAX - digit) / 10U)
      return 0;
    value = value * 10U + digit;
    ++cursor;
  }
  return value;
}

bool DeviceConfigCache::extract_float_(const std::string &json,
                                       const std::string &key, float *value) {
  if (value == nullptr)
    return false;
  const std::string needle = "\"" + key + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos)
    return false;
  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos)
    return false;
  size_t cursor = colon_pos + 1;
  while (cursor < json.size() &&
         std::isspace(static_cast<unsigned char>(json[cursor])))
    ++cursor;
  if (cursor >= json.size())
    return false;
  char *end = nullptr;
  const float parsed = std::strtof(json.c_str() + cursor, &end);
  if (end == json.c_str() + cursor || !std::isfinite(parsed))
    return false;
  *value = parsed;
  return true;
}

std::string DeviceConfigCache::extract_object_by_id_(const std::string &json, const std::string &id) {
  const std::string needle = "\"id\":\"" + id + "\"";
  const size_t id_pos = json.find(needle);
  if (id_pos == std::string::npos)
    return "";

  const size_t object_start = json.rfind('{', id_pos);
  if (object_start == std::string::npos)
    return "";

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (size_t pos = object_start; pos < json.size(); pos++) {
    const char current = json[pos];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (current == '\\' && in_string) {
      escaped = true;
      continue;
    }
    if (current == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;

    if (current == '{') {
      depth++;
    } else if (current == '}') {
      depth--;
      if (depth == 0)
        return json.substr(object_start, pos - object_start + 1);
      if (depth < 0)
        return "";
    }
  }

  return "";
}

std::string DeviceConfigCache::extract_object_by_key_(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos)
    return "";
  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos)
    return "";
  const size_t object_start = json.find('{', colon_pos + 1);
  if (object_start == std::string::npos)
    return "";

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (size_t pos = object_start; pos < json.size(); pos++) {
    const char current = json[pos];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (current == '\\' && in_string) {
      escaped = true;
      continue;
    }
    if (current == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;

    if (current == '{') {
      depth++;
    } else if (current == '}') {
      depth--;
      if (depth == 0)
        return json.substr(object_start, pos - object_start + 1);
      if (depth < 0)
        return "";
    }
  }
  return "";
}

std::string DeviceConfigCache::extract_string_(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  const size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos)
    return "";
  const size_t colon_pos = json.find(':', key_pos + needle.size());
  if (colon_pos == std::string::npos)
    return "";
  size_t value_start = json.find('"', colon_pos + 1);
  if (value_start == std::string::npos)
    return "";
  value_start++;

  bool escaped = false;
  for (size_t pos = value_start; pos < json.size(); pos++) {
    const char current = json[pos];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (current == '\\') {
      escaped = true;
      continue;
    }
    if (current == '"')
      return json.substr(value_start, pos - value_start);
  }
  return "";
}

int DeviceConfigCache::extract_boundary_points_(const std::string &json,
                                                float xs[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS],
                                                float ys[MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS]) {
  const size_t key_pos = json.find("\"boundary\"");
  if (key_pos == std::string::npos)
    return 0;
  const size_t end = json.size();
  size_t pos = json.find('[', key_pos);
  if (pos == std::string::npos || pos >= end)
    return 0;

  auto read_number = [&](size_t &cursor, float &value) -> bool {
    while (cursor < end && !(json[cursor] == '-' || json[cursor] == '+' ||
                             (json[cursor] >= '0' && json[cursor] <= '9')))
      cursor++;
    if (cursor >= end)
      return false;
    char *number_end = nullptr;
    value = std::strtof(json.c_str() + cursor, &number_end);
    if (number_end == json.c_str() + cursor)
      return false;
    cursor = static_cast<size_t>(number_end - json.c_str());
    return true;
  };

  int point_count = 0;
  while (pos < end && point_count < MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS) {
    float x = 0.0f;
    float y = 0.0f;
    if (!read_number(pos, x))
      break;
    if (!read_number(pos, y))
      break;
    xs[point_count] = x;
    ys[point_count] = y;
    point_count++;

    const size_t next = json.find('[', pos);
    const size_t boundary_close = json.find("]]", pos);
    if (next == std::string::npos || (boundary_close != std::string::npos && boundary_close < next) || next >= end)
      break;
    pos = next;
  }
  return point_count;
}

}  // namespace radar_api_server
}  // namespace esphome
