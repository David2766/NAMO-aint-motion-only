#pragma once

#include "control_state.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class ControlHandler {
 public:
  explicit ControlHandler(ControlState *control_state) : control_state_(control_state) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 private:
  ControlState *control_state_;

  void handle_status_(AsyncWebServerRequest *request);
  void handle_status_led_(AsyncWebServerRequest *request);
  void handle_led_duration_(AsyncWebServerRequest *request);
  void handle_environment_correction_(AsyncWebServerRequest *request);
  void handle_temperature_offset_(AsyncWebServerRequest *request);
  void handle_humidity_offset_(AsyncWebServerRequest *request);
  void handle_timezone_(AsyncWebServerRequest *request);
  void handle_static_radar_tuning_status_(AsyncWebServerRequest *request);
  void handle_static_radar_tuning_session_(AsyncWebServerRequest *request);
  void handle_static_radar_tuning_gate_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
