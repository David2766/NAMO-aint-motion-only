#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

struct StaticRadarGateTuningState {
  float move_energy{0.0f};
  float still_energy{0.0f};
  float move_threshold{100.0f};
  float still_threshold{100.0f};
};

struct ControlState {
  static constexpr size_t STATIC_RADAR_GATE_COUNT = 9;

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
  std::string timezone{"Asia/Seoul"};
  bool has_timezone = false;

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
  bool pending_timezone = false;
  std::string requested_timezone{"Asia/Seoul"};

  bool static_radar_available = false;
  bool static_radar_tuning_active = false;
  bool static_radar_tuning_requested_active = false;
  bool pending_static_radar_tuning_session = false;
  uint32_t static_radar_tuning_keepalive_ms = 0;
  uint16_t static_radar_gate_resolution_mm = 750;
  std::array<StaticRadarGateTuningState, STATIC_RADAR_GATE_COUNT> static_radar_gates{};
  bool pending_static_radar_gate = false;
  uint8_t requested_static_radar_gate = 0;
  uint8_t requested_static_radar_sensitivity = 0;
};

}  // namespace radar_api_server
}  // namespace esphome
