#pragma once

#include "radar_storage.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class SystemHandler {
 public:
  explicit SystemHandler(RadarStorage *storage) : storage_(storage) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 protected:
  RadarStorage *storage_;

  void handle_status_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
