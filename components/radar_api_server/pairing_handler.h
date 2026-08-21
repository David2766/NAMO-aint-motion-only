#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class RadarApiServer;

class PairingHandler {
 public:
  explicit PairingHandler(RadarApiServer *server) : server_(server) {}

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 protected:
  RadarApiServer *server_;

  void handle_get_(AsyncWebServerRequest *request);
  void handle_post_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
