#pragma once

namespace esphome {
namespace radar_api_server {

struct ControlState {
  bool status_led_enabled = true;
  bool has_status_led_enabled = false;
  float led_blink_duration = 60.0f;
  bool has_led_blink_duration = false;
  bool environment_correction_enabled = true;
  bool has_environment_correction_enabled = false;
  float temperature_offset = 0.0f;
  bool has_temperature_offset = false;
  float humidity_offset = 0.0f;
  bool has_humidity_offset = false;

  bool pending_status_led_enabled = false;
  bool requested_status_led_enabled = true;
  bool pending_led_blink_duration = false;
  float requested_led_blink_duration = 60.0f;
  bool pending_environment_correction_enabled = false;
  bool requested_environment_correction_enabled = true;
  bool pending_temperature_offset = false;
  float requested_temperature_offset = 0.0f;
  bool pending_humidity_offset = false;
  float requested_humidity_offset = 0.0f;
};

}  // namespace radar_api_server
}  // namespace esphome
