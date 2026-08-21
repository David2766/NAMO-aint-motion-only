#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class DashboardHandler {
 public:
  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url) const;
};

}  // namespace radar_api_server
}  // namespace esphome
