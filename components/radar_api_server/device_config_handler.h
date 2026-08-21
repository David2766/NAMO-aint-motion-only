#pragma once

#include "device_config_cache.h"
#include "radar_storage.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

class DeviceConfigHandler {
 public:
  explicit DeviceConfigHandler(RadarStorage *storage, DeviceConfigCache *cache) : storage_(storage), cache_(cache) {}

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 private:
  RadarStorage *storage_;
  DeviceConfigCache *cache_;

  void handle_status_(AsyncWebServerRequest *request);
  void handle_get_config_(AsyncWebServerRequest *request);
  void handle_upload_start_(AsyncWebServerRequest *request);
  void handle_upload_chunk_(AsyncWebServerRequest *request);
  void handle_upload_commit_(AsyncWebServerRequest *request);

  bool parse_upload_session_(AsyncWebServerRequest *request, uint32_t *session_id) const;
  bool decode_hex_(const std::string &hex, std::string *out) const;
};

}  // namespace radar_api_server
}  // namespace esphome
