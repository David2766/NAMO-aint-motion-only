#pragma once

#include <array>
#include <string>

namespace esphome {
namespace radar_api_server {

class DeviceConfigCache {
 public:
  void update(const std::string &json);
  void clear();

  bool has_config() const { return this->has_config_; }
  const std::string &raw_config() const { return this->raw_config_; }
  const std::string &software_zone_config(int zone_index) const;
  const std::string &calibration_zone_config(int zone_index) const;

 private:
  bool has_config_{false};
  std::string raw_config_;
  std::array<std::string, 6> software_zones_{};
  std::array<std::string, 4> calibration_zones_{};

  static const std::string EMPTY_;

  void refresh_zone_cache_();
  static std::string extract_object_by_id_(const std::string &json, const std::string &id);
};

}  // namespace radar_api_server
}  // namespace esphome
