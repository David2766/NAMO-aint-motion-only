#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

#include "../../../components/radar_api_server/device_config_cache.h"
#include "../../../components/radar_api_server/presence_fusion.h"
#include "../../../components/radar_api_server/presence_replay_log.h"
#include "../../../components/radar_api_server/presence_tracker.h"
#include "../../../components/radar_api_server/software_zone_evidence.h"
#include "../../../components/radar_api_server/state_json_builder.h"
#include "../../../components/radar_api_server/timezone_catalog.h"

namespace {

using esphome::radar_api_server::DeviceConfigCache;
using esphome::radar_api_server::PresenceFusion;
using esphome::radar_api_server::PresenceFusionInput;
using esphome::radar_api_server::PresenceReplayLog;
using esphome::radar_api_server::PresenceReplayRawInput;
using esphome::radar_api_server::PresenceTracker;
using esphome::radar_api_server::PresenceTrackerInput;
using esphome::radar_api_server::PresenceTrackerOutput;
using esphome::radar_api_server::SoftwareZoneTarget;
using esphome::radar_api_server::DeviceStateJsonInput;
using esphome::radar_api_server::DiagnosticSnapshot;
using esphome::radar_api_server::SoftwareZoneEvidence;
using esphome::radar_api_server::build_device_state_json;
using esphome::radar_api_server::compute_software_zone_evidence;
using esphome::radar_api_server::find_timezone;
using esphome::radar_api_server::is_supported_timezone;
using esphome::radar_api_server::timezone_posix;

PresenceTrackerInput make_input(uint32_t now_ms) {
  PresenceTrackerInput input;
  input.now_ms = now_ms;
  input.stationary_speed_cm_s = 40.0f;
  return input;
}

PresenceTrackerInput make_target_input(uint32_t now_ms, bool room_inside, bool room_outside,
                                       float room_signed_distance_mm, int exit_zone_mask = 0) {
  PresenceTrackerInput input = make_input(now_ms);
  input.room_context_configured = true;
  input.targets[0].valid = true;
  input.targets[0].x_mm = 1200.0f;
  input.targets[0].y_mm = 2200.0f;
  input.targets[0].distance_mm = 2500.0f;
  input.targets[0].speed_cm_s = 0.0f;
  input.targets[0].exit_zone_mask = exit_zone_mask;
  input.targets[0].room_inside = room_inside;
  input.targets[0].room_outside = room_outside;
  input.targets[0].room_signed_distance_mm = room_signed_distance_mm;
  return input;
}

void set_target(PresenceTrackerInput &input, int index, float x_mm, float y_mm) {
  input.targets[index].valid = true;
  input.targets[index].x_mm = x_mm;
  input.targets[index].y_mm = y_mm;
  input.targets[index].distance_mm = std::sqrt(x_mm * x_mm + y_mm * y_mm);
}

PresenceTrackerInput make_position_input(uint32_t now_ms, float x_mm, float y_mm, bool pir_motion = false) {
  PresenceTrackerInput input = make_input(now_ms);
  input.pir_motion = pir_motion;
  set_target(input, 0, x_mm, y_mm);
  return input;
}

PresenceTrackerInput make_two_target_input(uint32_t now_ms, float first_x_mm, float first_y_mm, float second_x_mm,
                                           float second_y_mm) {
  PresenceTrackerInput input = make_input(now_ms);
  set_target(input, 0, first_x_mm, first_y_mm);
  set_target(input, 1, second_x_mm, second_y_mm);
  return input;
}

void confirm_track(PresenceTracker &tracker, bool room_inside, bool room_outside, int exit_zone_mask = 0) {
  const float room_distance = room_inside ? 1200.0f : room_outside ? -1200.0f : 0.0f;
  tracker.update(make_target_input(500, room_inside, room_outside, room_distance, exit_zone_mask));
  tracker.update(make_target_input(1000, room_inside, room_outside, room_distance, exit_zone_mask));
  tracker.update(make_target_input(1500, room_inside, room_outside, room_distance, exit_zone_mask));
}

void move_track_outside_room(PresenceTracker &tracker) {
  tracker.update(make_target_input(2000, false, true, -1200.0f));
}

void miss_frames(PresenceTracker &tracker, int frames, uint32_t start_ms = 2500) {
  for (int i = 0; i < frames; i++) {
    tracker.update(make_input(start_ms + static_cast<uint32_t>(i) * 500U));
  }
}

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::cerr << "FAIL: " << message << "\n";
  std::exit(1);
}

void require_near(float actual, float expected, float tolerance, const char *message) {
  require(std::isfinite(actual), message);
  require(std::fabs(actual - expected) <= tolerance, message);
}

const esphome::radar_api_server::PresenceTrackerTrackOutput &require_track(const PresenceTrackerOutput &output,
                                                                           int index) {
  require(index >= 0 && index < static_cast<int>(output.tracks.size()), "requested output track must exist");
  require(output.tracks[index].valid, "requested output track must be valid");
  return output.tracks[index];
}

void no_detection_stays_idle() {
  PresenceTracker tracker;
  tracker.update(make_input(500));

  const auto &out = tracker.output();
  require(!out.presence, "empty input should not report presence");
  require(!out.motion, "empty input should not report motion");
  require(out.input_detection_count == 0, "empty input should report no detections");
  require(out.active_track_count == 0, "empty input should not allocate tracks");
  require(std::strcmp(out.state, "idle") == 0, "empty input should remain idle");
}

void confirmation_requires_configured_hit_count() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  require(!tracker.output().presence, "first hit should remain tentative");
  require(tracker.output().tentative_track_count == 1, "first hit should create one tentative track");

  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  require(!tracker.output().presence, "second hit without PIR should remain tentative");

  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  require(tracker.output().presence, "third hit should confirm presence");
  require(tracker.output().confirmed_track_count == 1, "third hit should confirm one track");
  require(std::strcmp(tracker.output().reason, "confirmed_by_hits") == 0,
          "normal confirmation should expose confirmed_by_hits");
}

void pir_hint_confirms_on_second_hit() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f, true));
  require(tracker.output().tentative_track_count == 1, "first PIR-assisted hit should remain tentative");

  tracker.update(make_position_input(1000, 0.0f, 2000.0f, true));
  require(tracker.output().confirmed_track_count == 1, "second PIR-assisted hit should confirm the track");
  require(std::strcmp(tracker.output().reason, "confirmed_by_pir_hint") == 0,
          "PIR-assisted confirmation should expose its reason");
}

void pir_without_target_is_fusion_evidence_not_tracker_evidence() {
  PresenceTracker tracker;
  PresenceTrackerInput tracker_input = make_input(500);
  tracker_input.pir_motion = true;
  tracker.update(tracker_input);

  require(!tracker.output().presence, "PIR alone must not create spatial tracker presence");
  require(!tracker.output().motion, "PIR alone must not create spatial tracker motion");

  PresenceFusion fusion;
  PresenceFusionInput fusion_input;
  fusion_input.pir_motion = true;
  fusion.update(fusion_input, tracker.output());

  require(fusion.output().presence, "PIR should remain instant final presence evidence");
  require(fusion.output().motion, "PIR should remain instant final motion evidence when filters allow it");
  require(std::strcmp(fusion.output().presence_reason, "pir") == 0,
          "PIR final presence should expose its reason");
}

void fusion_preserves_filter_and_pir_precedence() {
  PresenceTrackerOutput tracker_output;
  tracker_output.presence = true;
  tracker_output.motion = true;

  PresenceFusion fusion;
  PresenceFusionInput input;
  input.filter_blocked = true;
  fusion.update(input, tracker_output);

  require(!fusion.output().presence, "filter block should suppress tracker presence");
  require(!fusion.output().motion, "filter block should suppress tracker motion");
  require(std::strcmp(fusion.output().presence_reason, "filter_blocked") == 0,
          "filtered tracker presence should expose filter_blocked");

  input.pir_motion = true;
  fusion.update(input, tracker_output);
  require(fusion.output().presence, "PIR should override filter block for presence");
  require(!fusion.output().motion, "filter block should still suppress PIR motion");
  require(std::strcmp(fusion.output().presence_reason, "pir") == 0,
          "PIR presence should keep precedence over filter reason");
  require(std::strcmp(fusion.output().motion_reason, "filter_blocked") == 0,
          "filtered PIR motion should expose filter_blocked");
}

void fusion_uses_tracker_evidence_and_drop_reason() {
  PresenceTrackerOutput tracker_output;
  tracker_output.presence = true;
  tracker_output.motion = true;
  tracker_output.drop_reason = "lost_after_exit";

  PresenceFusion fusion;
  PresenceFusionInput input;
  fusion.update(input, tracker_output);

  require(fusion.output().presence, "tracker presence should flow through fusion");
  require(fusion.output().motion, "tracker motion should flow through fusion");
  require(std::strcmp(fusion.output().presence_reason, "tracker_primary") == 0,
          "tracker presence should expose tracker_primary");
  require(std::strcmp(fusion.output().motion_reason, "tracker_motion") == 0,
          "tracker motion should expose tracker_motion");
  require(std::strcmp(fusion.output().presence_off_reason, "lost_after_exit") == 0,
          "fusion should preserve a concrete tracker drop reason");

  tracker_output.presence = false;
  tracker_output.motion = false;
  tracker_output.drop_reason = "none";
  fusion.update(input, tracker_output);
  require(std::strcmp(fusion.output().presence_off_reason, "tracker_lost_hold_expired") == 0,
          "fusion should preserve the existing missing drop-reason fallback");
}

void static_radar_cannot_acquire_presence_by_itself() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;
  input.static_radar.still = true;
  input.static_radar.detection_distance_mm = 1850.0f;
  input.static_radar.still_distance_mm = 1850.0f;
  input.static_radar.still_energy = 67.0f;

  fusion.update(input, tracker_output);

  require(!fusion.output().presence, "static radar alone must not acquire production presence");
  require(!fusion.output().motion, "static radar alone must not create production motion");
  require(fusion.output().static_radar.available, "static radar availability should reach diagnostic output");
  require(fusion.output().static_radar.still, "static radar still evidence should reach diagnostic output");
  require(fusion.output().static_radar.detection_distance_mm == 1850.0f,
          "static radar detection distance should reach diagnostic output");
  require(fusion.output().static_radar.still_distance_mm == 1850.0f,
          "static radar distance should reach diagnostic output");
  require(std::strcmp(fusion.output().static_radar_reason, "still") == 0,
          "static radar diagnostic output should expose a stable reason");

  input.static_radar = {};
  fusion.update(input, tracker_output);
  require(std::strcmp(fusion.output().static_radar_reason, "unavailable") == 0,
          "missing static radar heartbeat should be reported as unavailable");
}

void static_radar_detection_distance_rejects_single_sample_spikes() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;

  input.now_ms = 1000;
  input.static_radar.detection_distance_mm = 1000.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 1000.0f,
          "the first valid static distance should be available immediately");

  input.now_ms = 2000;
  input.static_radar.detection_distance_mm = 3500.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 1000.0f,
          "one new distance sample should not replace the stable distance");

  input.now_ms = 3000;
  input.static_radar.detection_distance_mm = 1050.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 1050.0f,
          "the three-sample median should reject a single distance spike");
}

void static_radar_detection_distance_accepts_persistent_changes_and_resets() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;

  input.now_ms = 1000;
  input.static_radar.detection_distance_mm = 1000.0f;
  fusion.update(input, tracker_output);
  input.now_ms = 2000;
  input.static_radar.detection_distance_mm = 3500.0f;
  fusion.update(input, tracker_output);
  input.now_ms = 3000;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 3500.0f,
          "two consecutive new samples should move the stable distance");

  input.now_ms = 3500;
  input.static_radar.presence = false;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 0.0f,
          "cleared static presence should reset the stable distance");

  input.now_ms = 4000;
  input.static_radar.presence = true;
  input.static_radar.detection_distance_mm = 2200.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 2200.0f,
          "the first distance after reset should not mix with the previous session");
}

void static_radar_detection_distance_ignores_duplicate_ticks_and_unavailable_resets() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;

  input.now_ms = 1000;
  input.static_radar.detection_distance_mm = 1000.0f;
  fusion.update(input, tracker_output);

  input.now_ms = 1500;
  input.static_radar.detection_distance_mm = 3500.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 1000.0f,
          "a duplicate 500 ms tick must not enter the one-second distance window");

  input.now_ms = 2000;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 1000.0f,
          "one accepted replacement sample should not move the stable distance");

  input.now_ms = 3000;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 3500.0f,
          "two accepted replacement samples should move the stable distance");

  input.now_ms = 3500;
  input.static_radar.available = false;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 0.0f,
          "unavailable static radar should reset the stable distance");

  input.now_ms = 4000;
  input.static_radar.available = true;
  input.static_radar.detection_distance_mm = 2200.0f;
  fusion.update(input, tracker_output);
  require(fusion.output().static_radar.detection_distance_mm == 2200.0f,
          "the first sample after availability returns should start a new window");
}

void static_radar_detection_distance_keeps_stable_value_during_invalid_samples() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;

  input.now_ms = 1000;
  input.static_radar.detection_distance_mm = 1800.0f;
  fusion.update(input, tracker_output);

  input.now_ms = 2000;
  input.static_radar.detection_distance_mm = std::numeric_limits<float>::quiet_NaN();
  fusion.update(input, tracker_output);
  input.now_ms = 3000;
  input.static_radar.detection_distance_mm = 0.0f;
  fusion.update(input, tracker_output);

  require(fusion.output().static_radar.detection_distance_mm == 1800.0f,
          "brief invalid distance samples should not erase a stable distance while presence remains active");
}

void replay_log_serializes_canonical_static_distance() {
  PresenceReplayLog replay_log;
  PresenceTrackerInput tracker_input = make_input(1000);
  PresenceReplayRawInput raw_input;
  DiagnosticSnapshot snapshot;
  snapshot.ms = 1000;
  snapshot.static_radar.available = true;
  snapshot.static_radar.presence = true;
  snapshot.static_radar.still = true;
  snapshot.static_radar.moving_distance_mm = 0.0f;
  snapshot.static_radar.still_distance_mm = 1900.0f;
  snapshot.static_radar.moving_energy = 0.0f;
  snapshot.static_radar.still_energy = 67.0f;
  snapshot.static_radar.detection_distance_mm = 1850.0f;

  replay_log.update(tracker_input, raw_input, snapshot);

  char line[2048]{};
  require(replay_log.format_ndjson_sample(0, line, sizeof(line)),
          "replay sample should fit in the diagnostics line buffer");
  require(std::string(line).find("\"sr\":[1,1,0,1,0,1900,0,67,1850]") != std::string::npos,
          "replay sr tuple should append the stabilized canonical distance");
}

void arm_static_assist(PresenceFusion &fusion, PresenceFusionInput &input,
                       PresenceTrackerOutput &tracker_output) {
  input.static_radar.available = true;
  input.static_radar.presence = true;
  input.static_radar.still = true;
  input.pir_motion = true;
  input.stable_presence = false;
  input.now_ms = 500;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_arm_pending,
          "static assist should begin arming while primary and static evidence overlap");

  input.stable_presence = true;
  for (input.now_ms = 1000; input.now_ms <= 2000; input.now_ms += 500)
    fusion.update(input, tracker_output);
  require(!fusion.output().static_assist_armed && fusion.output().static_assist_arm_pending,
          "static assist should not arm before the full two-second overlap");

  input.now_ms = 2500;
  fusion.update(input, tracker_output);

  require(fusion.output().static_assist_armed,
          "static assist should arm after two seconds of overlapping evidence");
  require(!fusion.output().static_assist_active,
          "static assist should not be active while PIR evidence remains present");
}

void static_assist_maintains_presence_after_unconfirmed_loss() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);

  require(fusion.output().presence, "armed static evidence should maintain presence after unconfirmed loss");
  require(fusion.output().static_assist_active, "static assist should report active while it maintains presence");
  require(std::strcmp(fusion.output().presence_reason, "static_radar_assist") == 0,
          "static-only maintenance should expose its own presence reason");
  require(!fusion.output().motion, "static assist must not synthesize motion");
}

void static_assist_preserves_one_spatial_target_with_distance_confidence() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  tracker_output.presence = true;
  tracker_output.confirmed_track_count = 1;
  tracker_output.tracks[0].valid = true;
  tracker_output.tracks[0].x_mm = 600.0f;
  tracker_output.tracks[0].y_mm = 1800.0f;
  input.static_radar.detection_distance_mm = 1950.0f;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output = {};
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);

  const auto matched = fusion.output().static_assist_held_target;
  require(fusion.output().static_assist_active, "test setup should enter static assist");
  require(matched.valid, "static assist should preserve one unambiguous spatial target");
  require(matched.distance_comparable && matched.distance_matched,
          "nearby static distance should keep the held target spatially credible");
  require(std::fabs(matched.x_mm - 600.0f) < 0.1f && std::fabs(matched.y_mm - 1800.0f) < 0.1f,
          "held target should retain the final tracker coordinate");

  input.now_ms = 3500;
  input.static_radar.detection_distance_mm = 3200.0f;
  fusion.update(input, tracker_output);
  input.now_ms = 4500;
  fusion.update(input, tracker_output);
  const auto uncertain = fusion.output().static_assist_held_target;
  require(uncertain.valid && uncertain.distance_comparable && !uncertain.distance_matched,
          "a distant static reading should retain the marker but mark its position uncertain");
}

void static_assist_does_not_guess_between_multiple_spatial_targets() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  tracker_output.presence = true;
  tracker_output.confirmed_track_count = 2;
  tracker_output.tracks[0].valid = true;
  tracker_output.tracks[0].y_mm = 1800.0f;
  tracker_output.tracks[1].valid = true;
  tracker_output.tracks[1].x_mm = 900.0f;
  tracker_output.tracks[1].y_mm = 2400.0f;
  input.static_radar.detection_distance_mm = 1800.0f;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output = {};
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);

  require(fusion.output().static_assist_active, "static evidence may still maintain presence after multiple tracks");
  require(!fusion.output().static_assist_held_target.valid,
          "static radar distance must not be assigned to one of multiple prior spatial targets");
}

void static_assist_combines_pir_and_tracker_reacquisition() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_active, "test setup should enter static assist");

  input.now_ms = 3500;
  input.pir_motion = true;
  fusion.update(input, tracker_output);
  require(fusion.output().presence && fusion.output().motion,
          "PIR evidence should add presence and motion while static evidence remains available");
  require(fusion.output().pir_evidence, "fusion should expose concurrent PIR evidence");
  require(!fusion.output().static_assist_active,
          "static maintenance should be unnecessary while PIR directly supplies presence");

  input.now_ms = 4000;
  input.pir_motion = false;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_active,
          "static maintenance should resume after PIR clears in the same occupied session");

  input.now_ms = 4500;
  tracker_output.presence = true;
  tracker_output.confirmed_track_count = 1;
  tracker_output.drop_reason = "none";
  tracker_output.drop_ms = 0;
  fusion.update(input, tracker_output);
  require(fusion.output().presence && fusion.output().tracker_evidence,
          "confirmed tracker evidence should coexist with armed static evidence");
  require(!fusion.output().static_assist_active,
          "static maintenance should be unnecessary while tracker presence is available");

  input.now_ms = 5000;
  input.pir_motion = true;
  tracker_output.motion = true;
  fusion.update(input, tracker_output);
  require(fusion.output().pir_evidence && fusion.output().tracker_evidence,
          "PIR and tracker evidence should remain independently visible when both are present");
  require(fusion.output().motion, "PIR and tracker motion evidence should keep the existing combined motion result");
}

void static_assist_respects_exit_veto_and_ignores_stale_drop_events() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_after_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);
  require(!fusion.output().presence, "confirmed exit should prevent a new static hold");
  require(fusion.output().static_assist_exit_veto, "confirmed exit should expose its assist veto");
  require(std::strcmp(fusion.output().presence_off_reason, "lost_after_exit") == 0,
          "confirmed exit should preserve its off reason while the veto is active");

  input.now_ms = 3500;
  tracker_output.presence = true;
  tracker_output.confirmed_track_count = 1;
  fusion.update(input, tracker_output);
  require(fusion.output().presence && !fusion.output().static_assist_exit_veto,
          "confirmed tracker reacquisition should clear the exit veto");

  input.now_ms = 4000;
  tracker_output.presence = false;
  tracker_output.confirmed_track_count = 0;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_active,
          "an already consumed exit drop must not be reapplied after reacquisition");
}

void static_assist_allows_one_reentry_window_after_stable_presence_ends() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_active, "test setup should enter static assist");

  input.now_ms = 3500;
  input.static_radar.presence = false;
  fusion.update(input, tracker_output);
  require(!fusion.output().presence && fusion.output().static_assist_armed,
          "a static clear should release raw presence but retain arming during the existing three-second tail");
  require(std::strcmp(fusion.output().presence_off_reason, "static_radar_clear") == 0,
          "a static clear should expose the correct off reason");

  input.now_ms = 6500;
  input.stable_presence = false;
  fusion.update(input, tracker_output);
  require(!fusion.output().presence && fusion.output().static_assist_armed,
          "final stable presence off should preserve an armed assist during the reentry window");

  input.now_ms = 36000;
  input.static_radar.presence = true;
  fusion.update(input, tracker_output);
  require(fusion.output().presence && fusion.output().static_assist_active,
          "static evidence returning within thirty seconds should restore presence");
}

void static_assist_reentry_window_expires_after_thirty_seconds() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);

  input.now_ms = 3500;
  input.static_radar.presence = false;
  fusion.update(input, tracker_output);

  input.now_ms = 6500;
  input.stable_presence = false;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_armed,
          "test setup should retain the armed assist at the start of the reentry window");

  input.now_ms = 36500;
  input.static_radar.presence = true;
  fusion.update(input, tracker_output);
  require(!fusion.output().presence && !fusion.output().static_assist_armed,
          "static evidence at or after thirty seconds must not start a new presence session");
}

void static_assist_reentry_window_respects_exit_and_filter_guards() {
  PresenceFusion exit_fusion;
  PresenceFusionInput exit_input;
  PresenceTrackerOutput exit_tracker_output;
  arm_static_assist(exit_fusion, exit_input, exit_tracker_output);

  exit_input.now_ms = 3000;
  exit_input.pir_motion = false;
  exit_tracker_output.drop_reason = "lost_after_exit";
  exit_tracker_output.drop_ms = exit_input.now_ms;
  exit_fusion.update(exit_input, exit_tracker_output);

  exit_input.now_ms = 6000;
  exit_input.stable_presence = false;
  exit_fusion.update(exit_input, exit_tracker_output);
  require(!exit_fusion.output().presence && !exit_fusion.output().static_assist_armed,
          "confirmed exit must prevent the static assist reentry window");

  PresenceFusion filter_fusion;
  PresenceFusionInput filter_input;
  PresenceTrackerOutput filter_tracker_output;
  arm_static_assist(filter_fusion, filter_input, filter_tracker_output);

  filter_input.now_ms = 3000;
  filter_input.pir_motion = false;
  filter_tracker_output.drop_reason = "lost_without_exit";
  filter_tracker_output.drop_ms = filter_input.now_ms;
  filter_fusion.update(filter_input, filter_tracker_output);

  filter_input.now_ms = 3500;
  filter_input.static_radar.presence = false;
  filter_fusion.update(filter_input, filter_tracker_output);

  filter_input.now_ms = 6500;
  filter_input.stable_presence = false;
  filter_fusion.update(filter_input, filter_tracker_output);

  filter_input.now_ms = 7000;
  filter_input.filter_blocked = true;
  filter_input.static_radar.presence = true;
  filter_fusion.update(filter_input, filter_tracker_output);
  require(!filter_fusion.output().presence && !filter_fusion.output().static_assist_armed,
          "filter block must cancel an open static assist reentry window");

  filter_input.now_ms = 7500;
  filter_input.filter_blocked = false;
  filter_fusion.update(filter_input, filter_tracker_output);
  require(!filter_fusion.output().presence,
          "static evidence alone must not restart after a filter-cancelled reentry window");
}

void static_assist_does_not_bypass_filter_or_missing_hardware() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);
  require(fusion.output().static_assist_active, "test setup should enter static assist");

  input.now_ms = 3500;
  input.filter_blocked = true;
  fusion.update(input, tracker_output);
  require(!fusion.output().presence && !fusion.output().static_assist_active,
          "static assist must not bypass the existing filter block");

  input.now_ms = 4000;
  input.filter_blocked = false;
  input.static_radar = {};
  fusion.update(input, tracker_output);
  require(!fusion.output().presence, "unavailable optional hardware must be ignored");
  require(std::strcmp(fusion.output().presence_off_reason, "static_radar_unavailable") == 0,
          "missing static radar heartbeat should explain why maintenance ended");
}

void state_json_exposes_static_radar_fusion_evidence() {
  DeviceConfigCache cache;
  SoftwareZoneEvidence zone_evidence;
  DeviceStateJsonInput input;
  input.static_radar.available = true;
  input.static_radar.presence = true;
  input.static_radar.still = true;
  input.static_radar.detection_distance_mm = 1850.0f;
  input.static_radar.still_distance_mm = 1850.0f;
  input.static_radar.still_energy = 67.0f;
  input.static_radar_reason = "still";
  input.pir_evidence = true;
  input.static_assist_armed = true;
  input.static_assist_active = true;
  input.static_assist_held_target = {true, true, false, 600.0f, 1800.0f, 1897.0f, 3200.0f, 1303.0f};

  const std::string json = build_device_state_json(cache, zone_evidence, "{}", input);
  require(json.find("\"staticRadar\":{\"available\":true") != std::string::npos,
          "state JSON should expose static radar availability");
  require(json.find("\"detectionDistanceMm\":1850") != std::string::npos,
          "state JSON should expose the canonical static radar detection distance");
  require(json.find("\"stillDistanceMm\":1850") != std::string::npos,
          "state JSON should expose static radar distance in millimeters");
  require(json.find("\"reason\":\"still\"") != std::string::npos,
          "state JSON should expose static radar reason");
  require(json.find("\"presenceEvidence\":{\"pir\":true,\"tracker\":false,\"staticAssist\":true}") !=
              std::string::npos,
          "state JSON should expose independent presence evidence");
  require(json.find("\"assist\":{\"armed\":true,\"active\":true") != std::string::npos,
          "state JSON should expose static assist state");
  require(json.find("\"heldTarget\":{\"id\":\"target_1\",\"x\":600,\"y\":1800") != std::string::npos,
          "state JSON should expose the bounded held target snapshot");
  require(json.find("\"distanceMatched\":false") != std::string::npos,
          "state JSON should expose held-target distance confidence");
}

void tentative_track_expires_after_miss_budget() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));

  tracker.update(make_input(1000));
  tracker.update(make_input(1500));
  tracker.update(make_input(2000));
  require(tracker.output().tentative_track_count == 1, "tentative track should survive its configured miss budget");

  tracker.update(make_input(2500));
  require(tracker.output().active_track_count == 0, "tentative track should expire after exceeding its miss budget");
  require(std::strcmp(tracker.output().state, "idle") == 0, "expired tentative track should return to idle");
}

void confirmed_track_coasts_and_reacquires() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  tracker.update(make_input(2000));
  require(tracker.output().coasting_track_count == 1, "a missed confirmed track should enter coasting");
  require(require_track(tracker.output(), 0).coasting, "coasting state should be exposed on track output");

  tracker.update(make_position_input(2500, 0.0f, 2000.0f));
  require(tracker.output().confirmed_track_count == 1, "a nearby detection should reacquire a coasting track");
  require(tracker.output().coasting_track_count == 0, "reacquired track should leave coasting");
  require(std::strcmp(tracker.output().reason, "confirmed_reacquired") == 0,
          "reacquisition should expose confirmed_reacquired");
}

void filter_block_ages_track_without_consuming_detection() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  PresenceTrackerInput blocked = make_position_input(2000, 0.0f, 2000.0f);
  blocked.filter_blocked = true;
  tracker.update(blocked);
  require(tracker.output().coasting_track_count == 1, "filter block should age rather than update a confirmed track");
  require(std::strcmp(tracker.output().reason, "filter_blocked_missed") == 0,
          "filter block should expose filter_blocked_missed");

  tracker.update(make_position_input(2500, 0.0f, 2000.0f));
  require(tracker.output().confirmed_track_count == 1, "track should reacquire after filter block clears");
}

void non_exit_track_uses_long_coasting_budget() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  miss_frames(tracker, 24, 2000);
  require(tracker.output().presence, "non-exit track should survive 24 missed frames");
  require(tracker.output().coasting_track_count == 1, "non-exit track should still be coasting at its budget");

  tracker.update(make_input(14000));
  require(!tracker.output().presence, "non-exit track should expire after exceeding 24 missed frames");
  require(std::strcmp(tracker.output().drop_reason, "lost_without_exit") == 0,
          "non-exit expiration should record lost_without_exit");
}

void target_slot_reordering_preserves_stable_tracks() {
  PresenceTracker tracker;
  tracker.update(make_two_target_input(500, -1000.0f, 2000.0f, 1000.0f, 2000.0f));
  tracker.update(make_two_target_input(1000, -1000.0f, 2000.0f, 1000.0f, 2000.0f));
  tracker.update(make_two_target_input(1500, -1000.0f, 2000.0f, 1000.0f, 2000.0f));
  require(tracker.output().confirmed_track_count == 2, "two stable detections should confirm two tracks");

  tracker.update(make_two_target_input(2000, 1000.0f, 2000.0f, -1000.0f, 2000.0f));
  const auto &out = tracker.output();
  require(out.confirmed_track_count == 2, "sensor slot reordering should keep both tracks confirmed");
  require(out.coasting_track_count == 0, "sensor slot reordering should not make stable tracks coast");
  bool has_left_track = false;
  bool has_right_track = false;
  for (const auto &track : out.tracks) {
    if (!track.valid)
      continue;
    has_left_track = has_left_track || track.x_mm < 0.0f;
    has_right_track = has_right_track || track.x_mm > 0.0f;
  }
  require(has_left_track, "left stable track should remain associated with the left detection");
  require(has_right_track, "right stable track should remain associated with the right detection");
}

void unmatched_detection_uses_free_track_slot() {
  PresenceTracker tracker;
  tracker.update(make_two_target_input(500, -1000.0f, 2000.0f, 1000.0f, 2000.0f));
  tracker.update(make_two_target_input(1000, -1000.0f, 2000.0f, 1000.0f, 2000.0f));
  tracker.update(make_two_target_input(1500, -1000.0f, 2000.0f, 1000.0f, 2000.0f));

  PresenceTrackerInput input = make_two_target_input(2000, -1000.0f, 2000.0f, 1000.0f, 2000.0f);
  set_target(input, 2, 0.0f, 5000.0f);
  tracker.update(input);
  require(tracker.output().confirmed_track_count == 2, "existing stable tracks should remain confirmed");
  require(tracker.output().tentative_track_count == 1, "unmatched detection should allocate the free track slot");
  require(tracker.output().active_track_count == 3, "all three hardware target slots should be represented");
}

void accepted_teleport_resets_filter_state() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  tracker.update(make_position_input(2000, 1600.0f, 2000.0f));
  const auto &track = require_track(tracker.output(), 0);
  require_near(track.x_mm, 1600.0f, 0.01f, "accepted teleport should reset filtered position to measurement");
  require_near(track.vx_mm_s, 0.0f, 0.01f, "accepted teleport should reset x velocity");
  require_near(track.vy_mm_s, 0.0f, 0.01f, "accepted teleport should reset y velocity");
}

void kalman_filter_smooths_measurement_and_caps_long_prediction() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  tracker.update(make_position_input(2000, 400.0f, 2000.0f));
  const auto updated = require_track(tracker.output(), 0);
  require(updated.x_mm > 0.0f && updated.x_mm < 400.0f,
          "Kalman update should smooth a non-teleport position measurement");
  require(std::isfinite(updated.vx_mm_s) && updated.vx_mm_s > 0.0f,
          "Kalman update should estimate finite positive x velocity");

  tracker.update(make_input(100000));
  const auto predicted = require_track(tracker.output(), 0);
  require_near(predicted.x_mm, updated.x_mm + updated.vx_mm_s * 2.0f, 0.5f,
               "long prediction should use the configured two-second dt cap");
  require(std::isfinite(predicted.y_mm) && std::isfinite(predicted.vy_mm_s),
          "long prediction should preserve finite filter output");
}

void radial_motion_sets_approaching_and_away_directions() {
  PresenceTracker approaching;
  approaching.update(make_position_input(500, 0.0f, 3000.0f));
  approaching.update(make_position_input(1000, 0.0f, 2500.0f));
  approaching.update(make_position_input(1500, 0.0f, 2000.0f));
  require(std::strcmp(require_track(approaching.output(), 0).direction, "approaching") == 0,
          "decreasing radial distance should report approaching");

  PresenceTracker moving_away;
  moving_away.update(make_position_input(500, 0.0f, 1000.0f));
  moving_away.update(make_position_input(1000, 0.0f, 1500.0f));
  moving_away.update(make_position_input(1500, 0.0f, 2000.0f));
  require(std::strcmp(require_track(moving_away.output(), 0).direction, "moving_away") == 0,
          "increasing radial distance should report moving_away");
}

void room_exit_sequence_expires_before_inside_room_hold() {
  PresenceTracker inside_tracker;
  confirm_track(inside_tracker, true, false);
  miss_frames(inside_tracker, 13);
  require(inside_tracker.output().presence, "inside-room coasting should still hold after 13 missed frames");
  require(std::strcmp(inside_tracker.output().drop_reason, "none") == 0,
          "inside-room coasting should not record a drop after 13 missed frames");

  PresenceTracker outside_tracker;
  confirm_track(outside_tracker, true, false);
  move_track_outside_room(outside_tracker);
  miss_frames(outside_tracker, 13);
  require(!outside_tracker.output().presence, "room-exit coasting should expire after 13 missed frames");
  require(std::strcmp(outside_tracker.output().drop_reason, "lost_after_room_exit") == 0,
          "room-exit coasting should record lost_after_room_exit");
}

void outside_without_crossing_does_not_create_room_exit() {
  PresenceTracker tracker;
  confirm_track(tracker, false, true);
  miss_frames(tracker, 13);
  require(tracker.output().presence, "outside-only track without inside-to-outside crossing should not fast-drop");
  require(std::strcmp(tracker.output().drop_reason, "none") == 0,
          "outside-only track should not record room-exit drop");
}

void exit_evidence_still_expires_fastest() {
  PresenceTracker tracker;
  confirm_track(tracker, true, false, 1);
  miss_frames(tracker, 5);
  require(!tracker.output().presence, "exit evidence should expire faster than room boundary coasting");
  require(std::strcmp(tracker.output().drop_reason, "lost_after_exit") == 0,
          "exit evidence should keep lost_after_exit reason");
}

void room_state_is_exposed_on_track_output() {
  PresenceTracker tracker;
  confirm_track(tracker, false, true);
  const auto &out = tracker.output();
  require(out.tracks[0].valid, "confirmed tracker should expose a valid output track");
  require(std::strcmp(out.tracks[0].room_state, "outside") == 0, "track output should expose outside room state");
}

void room_signed_distance_is_positive_inside_and_negative_outside() {
  DeviceConfigCache cache;
  cache.update(
      R"({"version":1,"zones":[],"floorplan":{"room":{"id":"room_1","name":"Room","source":"stored_room","boundary":[[-1000,1000],[1000,1000],[1000,3000],[-1000,3000]]}}})");

  require(cache.floorplan_room().configured, "room context should parse from device config");
  require(cache.floorplan_room().boundary_point_count == 4, "room context should parse four boundary points");
  require(cache.floorplan_room_signed_distance(0.0f, 2000.0f) > 900.0f,
          "inside point should have positive signed distance");
  require(cache.floorplan_room_signed_distance(0.0f, 3500.0f) < -400.0f,
          "outside point should have negative signed distance");
}

void software_zone_evidence_carries_room_signed_distance() {
  DeviceConfigCache cache;
  cache.update(
      R"({"version":1,"zones":[],"floorplan":{"room":{"id":"room_1","name":"Room","source":"stored_room","boundary":[[-1000,1000],[1000,1000],[1000,3000],[-1000,3000]]}}})");
  const SoftwareZoneTarget targets[3] = {
      {true, 0.0f, 2000.0f},
      {true, 0.0f, 3500.0f},
      {false, 0.0f, 0.0f},
  };
  uint32_t exit_last_seen_ms = 0;
  const auto evidence = compute_software_zone_evidence(cache, 1000, targets, &exit_last_seen_ms);

  require(evidence.room_context_configured, "software evidence should expose configured room context");
  require(evidence.target_room_inside[0], "inside target should be marked inside room");
  require(evidence.target_room_signed_distances[0] > 900.0f, "inside target should carry positive signed distance");
  require(evidence.target_room_outside[1], "outside target should be marked outside room");
  require(evidence.target_room_signed_distances[1] < -400.0f, "outside target should carry negative signed distance");
}

void timezone_catalog_accepts_only_supported_ids() {
  require(is_supported_timezone("Asia/Seoul"), "timezone catalog should include the default timezone");
  require(std::strcmp(timezone_posix("Asia/Seoul"), "KST-9") == 0,
          "timezone catalog should map IANA ids to embedded POSIX rules");
  require(std::strcmp(timezone_posix("America/Los_Angeles"), "PST8PDT,M3.2.0,M11.1.0") == 0,
          "timezone catalog should preserve daylight-saving rules");
  require(find_timezone("Moon/Sea_of_Tranquility") == nullptr,
          "timezone catalog should reject unsupported ids");
  require(timezone_posix(nullptr) == nullptr, "timezone catalog should reject null ids");
}

}  // namespace

int main() {
  no_detection_stays_idle();
  confirmation_requires_configured_hit_count();
  pir_hint_confirms_on_second_hit();
  pir_without_target_is_fusion_evidence_not_tracker_evidence();
  fusion_preserves_filter_and_pir_precedence();
  fusion_uses_tracker_evidence_and_drop_reason();
  static_radar_cannot_acquire_presence_by_itself();
  static_radar_detection_distance_rejects_single_sample_spikes();
  static_radar_detection_distance_accepts_persistent_changes_and_resets();
  static_radar_detection_distance_ignores_duplicate_ticks_and_unavailable_resets();
  static_radar_detection_distance_keeps_stable_value_during_invalid_samples();
  replay_log_serializes_canonical_static_distance();
  static_assist_maintains_presence_after_unconfirmed_loss();
  static_assist_preserves_one_spatial_target_with_distance_confidence();
  static_assist_does_not_guess_between_multiple_spatial_targets();
  static_assist_combines_pir_and_tracker_reacquisition();
  static_assist_respects_exit_veto_and_ignores_stale_drop_events();
  static_assist_allows_one_reentry_window_after_stable_presence_ends();
  static_assist_reentry_window_expires_after_thirty_seconds();
  static_assist_reentry_window_respects_exit_and_filter_guards();
  static_assist_does_not_bypass_filter_or_missing_hardware();
  state_json_exposes_static_radar_fusion_evidence();
  tentative_track_expires_after_miss_budget();
  confirmed_track_coasts_and_reacquires();
  filter_block_ages_track_without_consuming_detection();
  non_exit_track_uses_long_coasting_budget();
  target_slot_reordering_preserves_stable_tracks();
  unmatched_detection_uses_free_track_slot();
  accepted_teleport_resets_filter_state();
  kalman_filter_smooths_measurement_and_caps_long_prediction();
  radial_motion_sets_approaching_and_away_directions();
  room_signed_distance_is_positive_inside_and_negative_outside();
  software_zone_evidence_carries_room_signed_distance();
  timezone_catalog_accepts_only_supported_ids();
  room_exit_sequence_expires_before_inside_room_hold();
  outside_without_crossing_does_not_create_room_exit();
  exit_evidence_still_expires_fastest();
  room_state_is_exposed_on_track_output();
  std::cout << "tracker_tests: ok\n";
  return 0;
}
