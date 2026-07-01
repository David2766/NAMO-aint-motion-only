#pragma once

#include "device_config_cache.h"
#include "radar_storage.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class DeviceConfigHandler {
 public:
  explicit DeviceConfigHandler(RadarStorage *storage, DeviceConfigCache *cache) : storage_(storage), cache_(cache) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 private:
  RadarStorage *storage_;
  DeviceConfigCache *cache_;

  void handle_status_(AsyncWebServerRequest *request);
  void handle_get_config_(AsyncWebServerRequest *request);
  void handle_post_config_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
