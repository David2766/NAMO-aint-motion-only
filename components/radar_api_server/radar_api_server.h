#pragma once

#include "dashboard_handler.h"
#include "control_handler.h"
#include "control_state.h"
#include "device_config_cache.h"
#include "device_config_handler.h"
#include "diagnostic_handler.h"
#include "diagnostic_log.h"
#include "floorplan_handler.h"
#include "legacy_presence.h"
#include "presence_fusion.h"
#include "presence_tracker.h"
#include "presence_replay_log.h"
#include "radar_storage.h"
#include "setup_handler.h"
#include "software_zone_evidence.h"
#include "state_json_builder.h"
#include "state_handler.h"
#include "stats_store.h"
#include "stats_handler.h"
#include "system_handler.h"
#include "timezone_catalog.h"
#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <functional>
#include <array>
#include <string>

namespace esphome {
namespace radar_api_server {

class RadarApiServer : public Component, public AsyncWebHandler {
 public:
  explicit RadarApiServer(web_server_base::WebServerBase *base)
      : base_(base), floorplan_handler_(&storage_), device_config_handler_(&storage_, &device_config_cache_),
        stats_handler_(&stats_store_, &storage_), system_handler_(&storage_, &device_config_cache_, &stats_store_, this),
        state_handler_(&state_json_), diagnostic_handler_(&diagnostic_log_, &presence_replay_log_),
        control_handler_(&control_state_), setup_handler_(this) {}

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
  bool tracker_assist_presence_enabled() const { return this->device_config_cache_.tracker_assist_presence_enabled(); }
  bool legacy_presence_fallback_enabled() const {
    return this->device_config_cache_.legacy_presence_fallback_enabled();
  }
  void update_today_stats_json(const std::string &json) { this->stats_store_.update_today(json); }
  void record_heatmap_hit(float tx, float ty) { this->stats_store_.record_heatmap_hit(tx, ty); }
  bool save_finished_stats_day(const std::string &finished_day_json, const std::string &new_today_json) {
    return this->stats_store_.finish_day(finished_day_json, new_today_json);
  }
  bool reset_today_stats(const std::string &new_today_json) {
    return this->stats_store_.reset_today(new_today_json);
  }
  void update_state_json(const std::string &json) { this->state_json_ = json; }
  void update_device_state_json(const DeviceStateJsonInput &input) {
    this->state_json_ =
        build_device_state_json(this->device_config_cache_, this->software_zone_evidence_,
                                this->presence_tracker_.debug_json(), input);
  }
  void update_presence_tracker(uint32_t now_ms, bool pir_motion, bool filter_blocked, bool stable_presence,
                               bool room_context_configured,
                               const StaticRadarEvidence &static_radar, float illuminance_lux,
                               float stationary_speed_cm_s,
                               bool target_1_valid, float target_1_x, float target_1_y,
                               float target_1_speed, float target_1_distance, int target_1_exit_zone_mask,
                               bool target_1_room_inside, bool target_1_room_outside,
                               float target_1_room_signed_distance,
                               bool target_2_valid, float target_2_x, float target_2_y, float target_2_speed,
                               float target_2_distance, int target_2_exit_zone_mask,
                               bool target_2_room_inside, bool target_2_room_outside,
                               float target_2_room_signed_distance,
                               bool target_3_valid, float target_3_x, float target_3_y, float target_3_speed,
                               float target_3_distance, int target_3_exit_zone_mask,
                               bool target_3_room_inside, bool target_3_room_outside,
                               float target_3_room_signed_distance);
  void update_legacy_presence(uint32_t now_ms, bool pir_motion, bool filter_blocked, int target_count,
                              int moving_target_count, int still_target_count, float stationary_speed_cm_s,
                              bool target_1_valid, float target_1_x, float target_1_y, float target_1_speed,
                              bool target_2_valid, float target_2_x, float target_2_y, float target_2_speed,
                              bool target_3_valid, float target_3_x, float target_3_y, float target_3_speed);
  void disable_legacy_presence() { this->legacy_presence_.disable(); }
  const LegacyPresenceOutput &legacy_presence_output() const { return this->legacy_presence_.output(); }
  void update_presence_replay_raw_targets(bool target_1_raw_valid, float target_1_x, float target_1_y,
                                          float target_1_speed, float target_1_distance, int target_1_filter_mode,
                                          bool target_1_filtered, bool target_1_range_valid,
                                          bool target_2_raw_valid, float target_2_x, float target_2_y,
                                          float target_2_speed, float target_2_distance, int target_2_filter_mode,
                                          bool target_2_filtered, bool target_2_range_valid,
                                          bool target_3_raw_valid, float target_3_x, float target_3_y,
                                          float target_3_speed, float target_3_distance, int target_3_filter_mode,
                                          bool target_3_filtered, bool target_3_range_valid);
  void update_software_zone_evidence(uint32_t now_ms, bool target_1_valid, float target_1_x, float target_1_y,
                                     bool target_2_valid, float target_2_x, float target_2_y,
                                     bool target_3_valid, float target_3_x, float target_3_y);
  SoftwareZoneEvidence software_zone_evidence_for_targets(uint32_t now_ms, bool target_1_valid, float target_1_x,
                                                          float target_1_y, bool target_2_valid, float target_2_x,
                                                          float target_2_y, bool target_3_valid, float target_3_x,
                                                          float target_3_y) const;
  const SoftwareZoneEvidence &software_zone_evidence() const { return this->software_zone_evidence_; }
  std::string tracker_debug_json() const { return this->presence_tracker_.debug_json(); }
  const PresenceTrackerOutput &tracker_output() const { return this->presence_tracker_.output(); }
  const PresenceFusionOutput &presence_fusion_output() const { return this->presence_fusion_.output(); }

  // Setup Wi-Fi uses ESP-IDF directly so provisioning can keep the setup AP alive
  // while validating the selected STA network. ESPHome is synced only at finish/handoff.
  void connect_setup_wifi_deferred(std::string ssid, std::string password);
  void pause_setup_sta_deferred();
  void close_setup_ap_deferred();
  void close_setup_ap_after(uint32_t delay_ms, std::function<void()> before_close = {});
  void finish_setup_scan_after(uint32_t delay_ms);
  void finish_setup_scan_and_open_ap_after(uint32_t delay_ms);
  void open_setup_ap_deferred();
  void schedule_setup_auto_reboot();
  void reboot_deferred();
  void reboot_after(uint32_t delay_ms);
  void handoff_wifi_to_esphome_after(uint32_t delay_ms);
  void reset_wifi_credentials_and_reboot_after(uint32_t delay_ms);
  void update_control_status(bool status_led_enabled, float led_blink_duration, bool environment_correction_enabled,
                             float temperature_offset, float humidity_offset, const std::string &timezone) {
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
    this->control_state_.timezone = timezone;
    this->control_state_.has_timezone = true;
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
  bool take_timezone_request(std::string *timezone) {
    if (!this->control_state_.pending_timezone)
      return false;
    this->control_state_.pending_timezone = false;
    *timezone = this->control_state_.requested_timezone;
    return true;
  }
  bool take_static_radar_tuning_session_request(uint32_t now_ms, bool *active) {
    constexpr uint32_t tuning_timeout_ms = 15000;
    if (this->control_state_.static_radar_tuning_requested_active &&
        now_ms - this->control_state_.static_radar_tuning_keepalive_ms >= tuning_timeout_ms) {
      this->control_state_.static_radar_tuning_requested_active = false;
      this->control_state_.static_radar_tuning_keepalive_ms = 0;
      this->control_state_.pending_static_radar_tuning_session = true;
    }
    if (!this->control_state_.pending_static_radar_tuning_session)
      return false;
    this->control_state_.pending_static_radar_tuning_session = false;
    *active = this->control_state_.static_radar_tuning_requested_active;
    return true;
  }
  bool take_static_radar_gate_request(uint8_t *gate, uint8_t *sensitivity) {
    if (!this->control_state_.pending_static_radar_gate)
      return false;
    this->control_state_.pending_static_radar_gate = false;
    *gate = this->control_state_.requested_static_radar_gate;
    *sensitivity = this->control_state_.requested_static_radar_sensitivity;
    return true;
  }
  void update_static_radar_tuning_status(
      bool available, bool active, uint16_t resolution_mm,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &move_energy,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &still_energy,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &move_threshold,
      const std::array<float, ControlState::STATIC_RADAR_GATE_COUNT> &still_threshold) {
    this->control_state_.static_radar_available = available;
    this->control_state_.static_radar_tuning_active = available && active;
    this->control_state_.static_radar_gate_resolution_mm = resolution_mm;
    if (!this->control_state_.static_radar_tuning_active)
      return;
    for (size_t gate = 0; gate < ControlState::STATIC_RADAR_GATE_COUNT; gate++) {
      this->control_state_.static_radar_gates[gate] = {
          move_energy[gate], still_energy[gate], move_threshold[gate], still_threshold[gate]};
    }
  }
  void update_diagnostic_snapshot(bool presence, bool motion, bool pir_motion, int target_count,
                                  int moving_target_count, int still_target_count, int still_confidence,
                                  bool still_hold_active, int empty_samples, int range_suspect_count,
                                  int range_out_of_range_count, int range_remote_candidate_count,
                                  float illuminance_lux, const char *presence_reason,
                                  const char *presence_off_reason, const char *motion_reason,
                                  const char *still_state, const char *still_reason,
                                  const char *range_reason, bool exit_zone_active, int exit_zone_mask,
                                  int exit_target_count, int exit_last_seen_age_ms);

 protected:
  web_server_base::WebServerBase *base_;
  RadarStorage storage_;
  DeviceConfigCache device_config_cache_;
  StatsStore stats_store_;
  ControlState control_state_;
  DiagnosticLog diagnostic_log_;
  PresenceTracker presence_tracker_;
  PresenceFusion presence_fusion_;
  LegacyPresence legacy_presence_;
  PresenceTrackerInput last_tracker_input_{};
  PresenceReplayRawInput last_replay_raw_input_{};
  PresenceReplayLog presence_replay_log_;
  SoftwareZoneEvidence software_zone_evidence_{};
  uint32_t exit_zone_last_seen_ms_{0};
  std::string state_json_;
  DashboardHandler dashboard_handler_;
  FloorplanHandler floorplan_handler_;
  DeviceConfigHandler device_config_handler_;
  StatsHandler stats_handler_;
  SystemHandler system_handler_;
  StateHandler state_handler_;
  DiagnosticHandler diagnostic_handler_;
  ControlHandler control_handler_;
  SetupHandler setup_handler_;
  bool ha_setup_handoff_started_{false};
};

}  // namespace radar_api_server
}  // namespace esphome
