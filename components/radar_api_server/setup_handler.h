#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class RadarApiServer;

class SetupHandler {
 public:
  explicit SetupHandler(RadarApiServer *server) : server_(server) {}

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);
  void prepare_setup_access_point() const;
  void finish_setup_scan() const;
  void finish_setup_scan_and_open_ap() const;

protected:
  void begin_setup_session_() const;
  void handle_root_(AsyncWebServerRequest *request) const;
  void handle_setup_page_(AsyncWebServerRequest *request) const;
  void handle_status_(AsyncWebServerRequest *request) const;
  void handle_networks_(AsyncWebServerRequest *request) const;
  void handle_scan_(AsyncWebServerRequest *request) const;
  void handle_prepare_security_(AsyncWebServerRequest *request) const;
  void handle_apply_wifi_(AsyncWebServerRequest *request) const;
  void handle_connected_(AsyncWebServerRequest *request) const;
  void handle_finish_(AsyncWebServerRequest *request) const;

  RadarApiServer *server_;
};

}  // namespace radar_api_server
}  // namespace esphome
