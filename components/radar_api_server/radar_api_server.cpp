#include "radar_api_server.h"

#include "http_response.h"
#include "esphome/core/log.h"

#include <utility>

namespace esphome {
namespace radar_api_server {

static const char *const TAG = "radar_api_server";

void RadarApiServer::setup() {
  this->storage_.ensure_partition();
  std::string config;
  if (this->storage_.read_payload(RadarPayloadTarget::DEVICE_CONFIG, &config)) {
    this->device_config_cache_.update(std::move(config));
  } else {
    this->device_config_cache_.clear();
  }
  this->stats_store_.load(&this->storage_);
  this->base_->add_handler(this);
  this->set_timeout("setup_prepare_access_point", 30 * 1000, [this]() {
    this->setup_handler_.prepare_setup_access_point();
  });
}

void RadarApiServer::dump_config() {
  ESP_LOGCONFIG(TAG, "Radar API Server:");
  ESP_LOGCONFIG(TAG, "  Setup: /setup");
  ESP_LOGCONFIG(TAG, "  Setup API: GET /api/setup/status|networks, POST /api/setup/prepare|apply-wifi|finish");
  ESP_LOGCONFIG(TAG, "  Dashboard: /dashboard");
  ESP_LOGCONFIG(TAG, "  Floorplan Status API: /api/floorplan/status");
  ESP_LOGCONFIG(TAG, "  Floorplan Config API: GET /api/floorplan");
  ESP_LOGCONFIG(TAG, "  Floorplan Image API: GET /api/floorplan/image");
  ESP_LOGCONFIG(TAG, "  Floorplan Patch API: POST /api/floorplan/radar|room-name|occlusion|objects");
  ESP_LOGCONFIG(TAG, "  Device Config Status API: /api/config/status");
  ESP_LOGCONFIG(TAG, "  Device Config API: GET /api/config");
  ESP_LOGCONFIG(TAG, "  Device Config Upload API: POST /api/config/upload/start|chunk|commit");
  ESP_LOGCONFIG(TAG, "  State API: GET /api/state");
  ESP_LOGCONFIG(TAG, "  Stats API: GET /api/stats");
  ESP_LOGCONFIG(TAG, "  Stats Upload API: POST /api/stats/upload/start|chunk|commit");
  ESP_LOGCONFIG(TAG, "  System API: GET /api/system/status, POST /api/system/reset");
  ESP_LOGCONFIG(TAG, "  Control Status API: GET /api/control/status");
  ESP_LOGCONFIG(TAG, "  Control API: POST /api/control/status-led|led-duration|environment-correction|temperature-offset|humidity-offset|timezone");
  ESP_LOGCONFIG(TAG, "  Static Radar Tuning API: GET status, POST session|gate");
  ESP_LOGCONFIG(TAG, "  Diagnostics API: GET /api/diagnostics/events|events.txt|replay.ndjson");
  ESP_LOGCONFIG(TAG, "  Floorplan Upload API: POST /api/floorplan/upload/start|chunk|commit");
}

float RadarApiServer::get_setup_priority() const { return setup_priority::DATA + 1.0f; }

bool RadarApiServer::canHandle(AsyncWebServerRequest *request) const {
  return this->setup_handler_.can_handle(request) || this->dashboard_handler_.can_handle(request) ||
         this->floorplan_handler_.can_handle(request) ||
         this->device_config_handler_.can_handle(request) || this->stats_handler_.can_handle(request) ||
         this->system_handler_.can_handle(request) || this->state_handler_.can_handle(request) ||
         this->diagnostic_handler_.can_handle(request) ||
         this->control_handler_.can_handle(request);
}

void RadarApiServer::handleRequest(AsyncWebServerRequest *request) {
  if (this->setup_handler_.handle(request))
    return;
  if (this->dashboard_handler_.handle(request))
    return;
  if (this->floorplan_handler_.handle(request))
    return;
  if (this->device_config_handler_.handle(request))
    return;
  if (this->stats_handler_.handle(request))
    return;
  if (this->system_handler_.handle(request))
    return;
  if (this->state_handler_.handle(request))
    return;
  if (this->diagnostic_handler_.handle(request))
    return;
  if (this->control_handler_.handle(request))
    return;

  http_response::send_error_info(request, 404, "not_found", "not_found", "error", "{}");
}

void RadarApiServer::update_presence_tracker(uint32_t now_ms, bool pir_motion, bool filter_blocked,
                                             bool stable_presence,
                                             bool room_context_configured,
                                             const StaticRadarEvidence &static_radar, float illuminance_lux,
                                             float stationary_speed_cm_s,
                                             bool target_1_valid, float target_1_x, float target_1_y,
                                             float target_1_speed, float target_1_distance,
                                             int target_1_exit_zone_mask, bool target_1_room_inside,
                                             bool target_1_room_outside, float target_1_room_signed_distance,
                                             bool target_2_valid, float target_2_x, float target_2_y,
                                             float target_2_speed, float target_2_distance,
                                             int target_2_exit_zone_mask, bool target_2_room_inside,
                                             bool target_2_room_outside, float target_2_room_signed_distance,
                                             bool target_3_valid, float target_3_x, float target_3_y,
                                             float target_3_speed, float target_3_distance,
                                             int target_3_exit_zone_mask, bool target_3_room_inside,
                                             bool target_3_room_outside, float target_3_room_signed_distance) {
  PresenceTrackerInput input;
  input.now_ms = now_ms;
  input.pir_motion = pir_motion;
  input.filter_blocked = filter_blocked;
  input.room_context_configured = room_context_configured;
  input.illuminance_lux = illuminance_lux;
  input.stationary_speed_cm_s = stationary_speed_cm_s;
  input.targets[0] = {target_1_valid, target_1_x, target_1_y, target_1_speed, target_1_distance,
                      target_1_exit_zone_mask, target_1_room_inside, target_1_room_outside,
                      target_1_room_signed_distance};
  input.targets[1] = {target_2_valid, target_2_x, target_2_y, target_2_speed, target_2_distance,
                      target_2_exit_zone_mask, target_2_room_inside, target_2_room_outside,
                      target_2_room_signed_distance};
  input.targets[2] = {target_3_valid, target_3_x, target_3_y, target_3_speed, target_3_distance,
                      target_3_exit_zone_mask, target_3_room_inside, target_3_room_outside,
                      target_3_room_signed_distance};
  this->last_tracker_input_ = input;
  this->presence_tracker_.update(input);
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = now_ms;
  fusion_input.pir_motion = pir_motion;
  fusion_input.filter_blocked = filter_blocked;
  fusion_input.stable_presence = stable_presence;
  fusion_input.static_radar = static_radar;
  this->presence_fusion_.update(fusion_input, this->presence_tracker_.output());
}

void RadarApiServer::update_legacy_presence(uint32_t now_ms, bool pir_motion, bool filter_blocked, int target_count,
                                            int moving_target_count, int still_target_count,
                                            float stationary_speed_cm_s, bool target_1_valid, float target_1_x,
                                            float target_1_y, float target_1_speed, bool target_2_valid,
                                            float target_2_x, float target_2_y, float target_2_speed,
                                            bool target_3_valid, float target_3_x, float target_3_y,
                                            float target_3_speed) {
  LegacyPresenceInput input;
  input.now_ms = now_ms;
  input.pir_motion = pir_motion;
  input.filter_blocked = filter_blocked;
  input.target_count = target_count;
  input.moving_target_count = moving_target_count;
  input.still_target_count = still_target_count;
  input.stationary_speed_cm_s = stationary_speed_cm_s;
  input.targets[0] = {target_1_valid, target_1_x, target_1_y, target_1_speed};
  input.targets[1] = {target_2_valid, target_2_x, target_2_y, target_2_speed};
  input.targets[2] = {target_3_valid, target_3_x, target_3_y, target_3_speed};
  this->legacy_presence_.update(input);
}

void RadarApiServer::update_presence_replay_raw_targets(
    bool target_1_raw_valid, float target_1_x, float target_1_y, float target_1_speed, float target_1_distance,
    int target_1_filter_mode, bool target_1_filtered, bool target_1_range_valid,
    bool target_2_raw_valid, float target_2_x, float target_2_y, float target_2_speed, float target_2_distance,
    int target_2_filter_mode, bool target_2_filtered, bool target_2_range_valid,
    bool target_3_raw_valid, float target_3_x, float target_3_y, float target_3_speed, float target_3_distance,
    int target_3_filter_mode, bool target_3_filtered, bool target_3_range_valid) {
  this->last_replay_raw_input_.targets[0] = {target_1_raw_valid, target_1_x, target_1_y, target_1_speed,
                                             target_1_distance, target_1_filter_mode, target_1_filtered,
                                             target_1_range_valid};
  this->last_replay_raw_input_.targets[1] = {target_2_raw_valid, target_2_x, target_2_y, target_2_speed,
                                             target_2_distance, target_2_filter_mode, target_2_filtered,
                                             target_2_range_valid};
  this->last_replay_raw_input_.targets[2] = {target_3_raw_valid, target_3_x, target_3_y, target_3_speed,
                                             target_3_distance, target_3_filter_mode, target_3_filtered,
                                             target_3_range_valid};
}

void RadarApiServer::update_software_zone_evidence(uint32_t now_ms, bool target_1_valid, float target_1_x,
                                                   float target_1_y, bool target_2_valid, float target_2_x,
                                                   float target_2_y, bool target_3_valid, float target_3_x,
                                                   float target_3_y) {
  const SoftwareZoneTarget targets[3] = {
      {target_1_valid, target_1_x, target_1_y},
      {target_2_valid, target_2_x, target_2_y},
      {target_3_valid, target_3_x, target_3_y},
  };
  this->software_zone_evidence_ =
      compute_software_zone_evidence(this->device_config_cache_, now_ms, targets, &this->exit_zone_last_seen_ms_);
}

SoftwareZoneEvidence RadarApiServer::software_zone_evidence_for_targets(uint32_t now_ms, bool target_1_valid,
                                                                        float target_1_x, float target_1_y,
                                                                        bool target_2_valid, float target_2_x,
                                                                        float target_2_y, bool target_3_valid,
                                                                        float target_3_x, float target_3_y) const {
  const SoftwareZoneTarget targets[3] = {
      {target_1_valid, target_1_x, target_1_y},
      {target_2_valid, target_2_x, target_2_y},
      {target_3_valid, target_3_x, target_3_y},
  };
  uint32_t scratch_exit_last_seen_ms = 0;
  return compute_software_zone_evidence(this->device_config_cache_, now_ms, targets, &scratch_exit_last_seen_ms);
}

void RadarApiServer::update_diagnostic_snapshot(bool presence, bool motion, bool pir_motion, int target_count,
                                                int moving_target_count, int still_target_count,
                                                int still_confidence, bool still_hold_active, int empty_samples,
                                                int range_suspect_count, int range_out_of_range_count,
                                                int range_remote_candidate_count, float illuminance_lux,
                                                const char *presence_reason, const char *presence_off_reason,
                                                const char *motion_reason, const char *still_state,
                                                const char *still_reason, const char *range_reason,
                                                bool exit_zone_active, int exit_zone_mask, int exit_target_count,
                                                int exit_last_seen_age_ms) {
  DiagnosticSnapshot snapshot;
  snapshot.ms = millis();
  snapshot.presence = presence;
  snapshot.motion = motion;
  snapshot.pir_motion = pir_motion;
  snapshot.target_count = target_count;
  snapshot.moving_target_count = moving_target_count;
  snapshot.still_target_count = still_target_count;
  snapshot.still_confidence = still_confidence;
  snapshot.still_hold_active = still_hold_active;
  snapshot.empty_samples = empty_samples;
  snapshot.range_suspect_count = range_suspect_count;
  snapshot.range_out_of_range_count = range_out_of_range_count;
  snapshot.range_remote_candidate_count = range_remote_candidate_count;
  snapshot.illuminance_lux = illuminance_lux;
  snapshot.presence_reason = presence_reason;
  snapshot.presence_off_reason = presence_off_reason;
  snapshot.motion_reason = motion_reason;
  snapshot.still_state = still_state;
  snapshot.still_reason = still_reason;
  snapshot.range_reason = range_reason;
  snapshot.exit_zone_active = exit_zone_active;
  snapshot.exit_zone_mask = exit_zone_mask;
  snapshot.exit_target_count = exit_target_count;
  snapshot.exit_last_seen_age_ms = exit_last_seen_age_ms;
  const auto &tracker = this->presence_tracker_.output();
  snapshot.tracker_presence = tracker.presence;
  snapshot.tracker_motion = tracker.motion;
  snapshot.tracker_track_score = tracker.track_score;
  snapshot.tracker_input_detection_count = tracker.input_detection_count;
  snapshot.tracker_active_track_count = tracker.active_track_count;
  snapshot.tracker_tentative_track_count = tracker.tentative_track_count;
  snapshot.tracker_confirmed_track_count = tracker.confirmed_track_count;
  snapshot.tracker_coasting_track_count = tracker.coasting_track_count;
  snapshot.tracker_moving_track_count = tracker.moving_track_count;
  snapshot.tracker_still_track_count = tracker.still_track_count;
  snapshot.tracker_state = tracker.state;
  snapshot.tracker_reason = tracker.reason;
  const auto &fusion = this->presence_fusion_.output();
  snapshot.static_radar = fusion.static_radar;
  snapshot.pir_evidence = fusion.pir_evidence;
  snapshot.tracker_evidence = fusion.tracker_evidence;
  snapshot.static_assist_armed = fusion.static_assist_armed;
  snapshot.static_assist_active = fusion.static_assist_active;
  snapshot.static_assist_arm_pending = fusion.static_assist_arm_pending;
  snapshot.static_assist_exit_veto = fusion.static_assist_exit_veto;
  snapshot.static_assist_arm_elapsed_ms = fusion.static_assist_arm_elapsed_ms;
  this->diagnostic_log_.update(snapshot);
  this->presence_replay_log_.update(this->last_tracker_input_, this->last_replay_raw_input_, snapshot);
}

}  // namespace radar_api_server
}  // namespace esphome
