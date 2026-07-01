#include "device_config_cache.h"

namespace esphome {
namespace radar_api_server {

const std::string DeviceConfigCache::EMPTY_{};

void DeviceConfigCache::update(const std::string &json) {
  this->raw_config_ = json;
  this->has_config_ = !json.empty();
  this->refresh_zone_cache_();
}

void DeviceConfigCache::clear() {
  this->raw_config_.clear();
  this->has_config_ = false;
  for (auto &zone : this->software_zones_)
    zone.clear();
  for (auto &zone : this->calibration_zones_)
    zone.clear();
}

const std::string &DeviceConfigCache::software_zone_config(int zone_index) const {
  if (zone_index < 1 || zone_index > static_cast<int>(this->software_zones_.size()))
    return EMPTY_;
  return this->software_zones_[zone_index - 1];
}

const std::string &DeviceConfigCache::calibration_zone_config(int zone_index) const {
  if (zone_index < 1 || zone_index > static_cast<int>(this->calibration_zones_.size()))
    return EMPTY_;
  return this->calibration_zones_[zone_index - 1];
}

void DeviceConfigCache::refresh_zone_cache_() {
  for (int index = 1; index <= static_cast<int>(this->software_zones_.size()); index++) {
    this->software_zones_[index - 1] = extract_object_by_id_(this->raw_config_, "zone_" + std::to_string(index));
  }
  for (int index = 1; index <= static_cast<int>(this->calibration_zones_.size()); index++) {
    this->calibration_zones_[index - 1] =
        extract_object_by_id_(this->raw_config_, "calibration_" + std::to_string(index));
  }
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

}  // namespace radar_api_server
}  // namespace esphome
