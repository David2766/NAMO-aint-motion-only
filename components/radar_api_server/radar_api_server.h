#pragma once

#include "dashboard_handler.h"
#include "control_handler.h"
#include "control_state.h"
#include "device_config_cache.h"
#include "device_config_handler.h"
#include "floorplan_handler.h"
#include "radar_storage.h"
#include "state_handler.h"
#include "stats_store.h"
#include "stats_handler.h"
#include "system_handler.h"
#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class RadarApiServer : public Component, public AsyncWebHandler {
 public:
  explicit RadarApiServer(web_server_base::WebServerBase *base)
      : base_(base), floorplan_handler_(&storage_), device_config_handler_(&storage_, &device_config_cache_),
        stats_handler_(&stats_store_, &storage_), system_handler_(&storage_), state_handler_(&state_json_),
        control_handler_(&control_state_) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  const std::string &software_zone_config(int zone_index) const {
    return this->device_config_cache_.software_zone_config(zone_index);
  }
  const std::string &calibration_zone_config(int zone_index) const {
    return this->device_config_cache_.calibration_zone_config(zone_index);
  }
  void update_today_stats_json(const std::string &json) { this->stats_store_.update_today(json); }
  void record_heatmap_hit(float tx, float ty) { this->stats_store_.record_heatmap_hit(tx, ty); }
  bool save_finished_stats_day(const std::string &finished_day_json, const std::string &new_today_json) {
    return this->stats_store_.finish_day(finished_day_json, new_today_json);
  }
  void update_state_json(const std::string &json) { this->state_json_ = json; }
  void update_control_status(bool status_led_enabled, float led_blink_duration, bool environment_correction_enabled,
                             float temperature_offset, float humidity_offset) {
    this->control_state_.status_led_enabled = status_led_enabled;
    this->control_state_.has_status_led_enabled = true;
    this->control_state_.led_blink_duration = led_blink_duration;
    this->control_state_.has_led_blink_duration = true;
    this->control_state_.environment_correction_enabled = environment_correction_enabled;
    this->control_state_.has_environment_correction_enabled = true;
    this->control_state_.temperature_offset = temperature_offset;
    this->control_state_.has_temperature_offset = true;
    this->control_state_.humidity_offset = humidity_offset;
    this->control_state_.has_humidity_offset = true;
  }
  bool take_status_led_request(bool *enabled) {
    if (!this->control_state_.pending_status_led_enabled)
      return false;
    this->control_state_.pending_status_led_enabled = false;
    *enabled = this->control_state_.requested_status_led_enabled;
    return true;
  }
  bool take_led_blink_duration_request(float *seconds) {
    if (!this->control_state_.pending_led_blink_duration)
      return false;
    this->control_state_.pending_led_blink_duration = false;
    *seconds = this->control_state_.requested_led_blink_duration;
    return true;
  }
  bool take_environment_correction_request(bool *enabled) {
    if (!this->control_state_.pending_environment_correction_enabled)
      return false;
    this->control_state_.pending_environment_correction_enabled = false;
    *enabled = this->control_state_.requested_environment_correction_enabled;
    return true;
  }
  bool take_temperature_offset_request(float *value) {
    if (!this->control_state_.pending_temperature_offset)
      return false;
    this->control_state_.pending_temperature_offset = false;
    *value = this->control_state_.requested_temperature_offset;
    return true;
  }
  bool take_humidity_offset_request(float *value) {
    if (!this->control_state_.pending_humidity_offset)
      return false;
    this->control_state_.pending_humidity_offset = false;
    *value = this->control_state_.requested_humidity_offset;
    return true;
  }

 protected:
  web_server_base::WebServerBase *base_;
  RadarStorage storage_;
  DeviceConfigCache device_config_cache_;
  StatsStore stats_store_;
  ControlState control_state_;
  std::string state_json_;
  DashboardHandler dashboard_handler_;
  FloorplanHandler floorplan_handler_;
  DeviceConfigHandler device_config_handler_;
  StatsHandler stats_handler_;
  SystemHandler system_handler_;
  StateHandler state_handler_;
  ControlHandler control_handler_;
};

}  // namespace radar_api_server
}  // namespace esphome
