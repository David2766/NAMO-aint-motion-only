#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

#include "../../../components/radar_api_server/device_config_cache.h"
#include "../../../components/radar_api_server/control_state.h"
#include "../../../components/radar_api_server/local_observation.h"
#include "../../../components/radar_api_server/presence_fusion.h"
#include "../../../components/radar_api_server/presence_replay_log.h"
#include "../../../components/radar_api_server/presence_tracker.h"
#include "../../../components/radar_api_server/platform_runtime_workspace.h"
#include "../../../components/radar_api_server/software_zone_evidence.h"
#include "../../../components/radar_api_server/state_json_builder.h"
#include "../../../components/radar_api_server/strict_parse.h"
#include "../../../components/radar_api_server/timezone_catalog.h"

void run_participant_registry_tests();
void run_participant_readiness_tests();
void run_participant_credentials_tests();
void run_participant_projection_tests();
void run_credential_provisioning_tests();
void run_participant_pairing_tests();
void run_pairing_bootstrap_activation_tests();
void run_participant_discovery_tests();
void run_platform_identity_tests();
void run_observation_transport_tests();
void run_group_coordinator_tests();
void run_group_membership_tests();
void run_group_roster_tests();
void run_site_coordinator_tests();
void run_site_configuration_tests();
void run_site_authority_tests();
void run_site_directory_tests();
void run_site_group_summary_tests();
void run_site_space_summary_tests();
void run_site_heartbeat_tests();
void run_site_replication_tests();
void run_ld2450_startup_guard_tests();
void run_ld245x_startup_sequence_tests();
void run_ld2410_stream_health_tests();
void run_spatial_shadow_fusion_tests();
void run_spatial_shadow_runtime_tests();
void run_spatial_production_fusion_tests();
void run_role_failure_matrix_tests();
void run_shared_automation_area_tests();
void run_firmware_update_security_tests();

namespace {

using esphome::radar_api_server::DeviceConfigCache;
using esphome::radar_api_server::LocalObservation;
using esphome::radar_api_server::LocalObservationAdapter;
using esphome::radar_api_server::LocalObservationRoomState;
using esphome::radar_api_server::PresenceFusion;
using esphome::radar_api_server::PresenceFusionInput;
using esphome::radar_api_server::PresenceReplayLog;
using esphome::radar_api_server::PresenceReplayRawInput;
using esphome::radar_api_server::PresenceTargetAdmissionInput;
using esphome::radar_api_server::PresenceTracker;
using esphome::radar_api_server::PresenceTrackerInput;
using esphome::radar_api_server::PresenceTrackerOutput;
using esphome::radar_api_server::PlatformParticipantOperation;
using esphome::radar_api_server::PlatformParticipantOperationWorkspace;
using esphome::radar_api_server::SoftwareZoneTarget;
using esphome::radar_api_server::ZonePolicyAction;
using esphome::radar_api_server::ZonePolicySource;
using esphome::radar_api_server::DeviceStateJsonInput;
using esphome::radar_api_server::DiagnosticSnapshot;
using esphome::radar_api_server::FusionAutomationState;
using esphome::radar_api_server::FusionSnapshot;
using esphome::radar_api_server::FixedTextBuffer;
using esphome::radar_api_server::SpatialProductionFusionOutput;
using esphome::radar_api_server::SpatialProductionFusionState;
using esphome::radar_api_server::SpatialShadowTrackState;
using esphome::radar_api_server::SpatialTransform2D;
using esphome::radar_api_server::SoftwareZoneEvidence;
using esphome::radar_api_server::build_device_state_json;
using esphome::radar_api_server::compute_software_zone_evidence;
using esphome::radar_api_server::compute_target_zone_policy;
using esphome::radar_api_server::evaluate_presence_target_admission;
using esphome::radar_api_server::find_timezone;
using esphome::radar_api_server::is_supported_timezone;
using esphome::radar_api_server::timezone_posix;

void require(bool condition, const char *message);

void platform_operation_lease_serializes_mutations() {
  PlatformParticipantOperationWorkspace workspace{};
  PlatformParticipantOperation operation{};
  operation.setup(&workspace);

  auto first = operation.try_acquire();
  require(first.acquired() && operation.busy(),
          "the first platform mutation lease must be acquired");
  auto overlapping = operation.try_acquire();
  require(!overlapping.acquired(),
          "overlapping platform mutations must be rejected");

  auto moved = std::move(first);
  require(!first.acquired() && moved.acquired() && operation.busy(),
          "moving a platform mutation lease must preserve ownership");
  moved = {};
  require(!operation.busy(),
          "releasing the platform mutation lease must clear busy state");

  auto next = operation.try_acquire();
  require(next.acquired(),
          "a completed platform mutation must not block the next operation");
}

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

PresenceTrackerInput make_reduced_position_input(uint32_t now_ms, float x_mm, float y_mm,
                                                 bool pir_motion = false) {
  PresenceTrackerInput input = make_position_input(now_ms, x_mm, y_mm, pir_motion);
  input.targets[0].reduced_zone = true;
  return input;
}

PresenceTrackerInput make_two_target_input(uint32_t now_ms, float first_x_mm, float first_y_mm, float second_x_mm,
                                           float second_y_mm) {
  PresenceTrackerInput input = make_input(now_ms);
  set_target(input, 0, first_x_mm, first_y_mm);
  set_target(input, 1, second_x_mm, second_y_mm);
  return input;
}

void confirm_two_tracks(PresenceTracker &tracker, float first_x_mm,
                        float first_y_mm, float second_x_mm,
                        float second_y_mm) {
  tracker.update(make_two_target_input(500, first_x_mm + 400.0f, first_y_mm,
                                       second_x_mm - 750.0f, second_y_mm));
  for (uint32_t now_ms = 1000; now_ms <= 2500; now_ms += 500)
    tracker.update(make_two_target_input(now_ms, first_x_mm, first_y_mm,
                                         second_x_mm, second_y_mm));
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

void reduced_zone_candidate_does_not_create_a_track() {
  PresenceTracker tracker;

  for (uint32_t now_ms = 500; now_ms <= 10000; now_ms += 500)
    tracker.update(make_reduced_position_input(now_ms, 0.0f, 2000.0f));

  require(!tracker.output().presence, "reduced-zone observations alone must not create presence");
  require(tracker.output().active_track_count == 0,
          "a reduced-zone observation must not allocate an internal track");
  require(tracker.output().tentative_track_count == 0,
          "a reduced-zone observation must not create a tentative track");
  require(tracker.output().confirmed_track_count == 0,
          "elapsed time must not promote a reduced-zone observation");
}

void pir_cannot_create_a_reduced_zone_track() {
  PresenceTracker tracker;

  for (uint32_t now_ms = 500; now_ms <= 5000; now_ms += 500)
    tracker.update(make_reduced_position_input(now_ms, 0.0f, 2000.0f, true));

  require(!tracker.output().presence,
          "PIR evidence must not grant a reduced-zone observation spatial admission");
  require(tracker.output().active_track_count == 0,
          "PIR evidence must not allocate a reduced-zone track");
}

void target_born_in_reduced_zone_starts_tracking_only_after_exit() {
  PresenceTracker tracker;

  tracker.update(make_reduced_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_reduced_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_reduced_position_input(1500, 0.0f, 2000.0f));
  require(tracker.output().active_track_count == 0,
          "a target first observed inside a reduced zone must remain untracked");

  tracker.update(make_position_input(2000, 100.0f, 2000.0f));
  tracker.update(make_position_input(2500, 200.0f, 2000.0f));
  tracker.update(make_position_input(3000, 300.0f, 2000.0f));

  require(tracker.output().presence, "normal observations should create a track after leaving the reduced zone");
  require(tracker.output().confirmed_track_count == 1,
          "the target should confirm through the normal outside-zone admission path");
  require(tracker.output().active_track_count == 1,
          "leaving the reduced zone should create exactly one track");
}

void confirmed_track_crosses_reduced_zone_without_changing_identity() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t track_id = tracker.output().tracks[0].local_track_id;
  for (uint32_t now_ms = 2000; now_ms <= 6000; now_ms += 500) {
    const float x_mm = static_cast<float>(now_ms - 1500) * 0.2f;
    tracker.update(make_reduced_position_input(now_ms, x_mm, 2000.0f));
    require(tracker.output().confirmed_track_count == 1,
            "an existing track must remain visible while crossing a reduced zone");
    require(tracker.output().tracks[0].local_track_id == track_id,
            "a reduced-zone crossing must preserve the existing track ID");
  }

  tracker.update(make_position_input(6500, 1000.0f, 2000.0f));
  require(tracker.output().confirmed_track_count == 1,
          "leaving the reduced zone must preserve confirmation");
  require(tracker.output().tracks[0].local_track_id == track_id,
          "a reduced-zone crossing must not replace the existing track");
  require(!tracker.output().tracks[0].reduced_zone,
          "a normal observation should clear the reduced-zone marker");
}

void persistent_reduced_zone_observation_expires_an_existing_track() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  for (uint32_t now_ms = 2000; now_ms <= 7000; now_ms += 500) {
    tracker.update(make_reduced_position_input(now_ms, 100.0f, 2000.0f, true));
    require(tracker.output().confirmed_track_count == 1,
            "an existing person should remain visible during the reduced-zone transit window");
  }

  for (uint32_t now_ms = 7500; now_ms <= 10000; now_ms += 500)
    tracker.update(make_reduced_position_input(now_ms, 100.0f, 2000.0f, true));

  require(!tracker.output().presence,
          "continuous reduced-zone evidence must not keep an established person forever");
  require(tracker.output().active_track_count == 0,
          "an expired reduced-zone track must be deleted even while PIR remains active");
}

void coasting_track_reacquires_inside_reduced_zone_without_pir() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t track_id = tracker.output().tracks[0].local_track_id;
  tracker.update(make_input(2000));
  tracker.update(make_reduced_position_input(2500, 100.0f, 2000.0f));

  require(tracker.output().confirmed_track_count == 1,
          "a reduced-zone observation should reacquire an existing coasting track");
  require(tracker.output().coasting_track_count == 0,
          "reacquisition should restore the existing track without PIR");
  require(tracker.output().tracks[0].local_track_id == track_id,
          "reduced-zone reacquisition must preserve the existing track ID");
}

void duplicate_reduced_observations_do_not_split_an_existing_track() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t track_id = tracker.output().tracks[0].local_track_id;

  PresenceTrackerInput duplicated = make_two_target_input(2000, 100.0f, 2000.0f, 250.0f, 2050.0f);
  duplicated.targets[0].reduced_zone = true;
  duplicated.targets[1].reduced_zone = true;
  tracker.update(duplicated);

  duplicated = make_two_target_input(2500, 300.0f, 2050.0f, 150.0f, 2000.0f);
  duplicated.targets[0].reduced_zone = true;
  duplicated.targets[1].reduced_zone = true;
  tracker.update(duplicated);

  require(tracker.output().active_track_count == 1,
          "duplicate reduced-zone observations must not allocate a second track");
  require(tracker.output().confirmed_track_count == 1,
          "the established person must remain the only confirmed track");
  require(tracker.output().tracks[0].local_track_id == track_id,
          "duplicate reduced-zone observations must preserve the established track ID");
}

void distant_reduced_zone_jump_does_not_move_or_replace_an_existing_track() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t track_id = tracker.output().tracks[0].local_track_id;

  tracker.update(make_reduced_position_input(2000, 1400.0f, 2000.0f));
  require(tracker.output().coasting_track_count == 1,
          "a distant reduced-zone jump should leave the established track coasting");
  require(tracker.output().tracks[0].local_track_id == track_id,
          "a rejected reduced-zone jump must preserve the established track ID");
  require(std::fabs(tracker.output().tracks[0].x_mm) < 500.0f,
          "a distant reduced-zone jump must not move the displayed coordinate");

  for (uint32_t now_ms = 2500; now_ms <= 14500; now_ms += 500)
    tracker.update(make_reduced_position_input(now_ms, 1400.0f, 2000.0f));

  require(tracker.output().active_track_count == 0,
          "repeated distant reduced-zone observations must not replace an expired track");
}

void reduced_zone_teleport_does_not_reanchor_a_transiting_track() {
  PresenceTracker tracker;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t track_id = tracker.output().tracks[0].local_track_id;
  tracker.update(make_reduced_position_input(2000, 200.0f, 2000.0f));
  const float accepted_x = tracker.output().tracks[0].x_mm;

  tracker.update(make_reduced_position_input(2500, 1800.0f, 2000.0f));
  tracker.update(make_reduced_position_input(3000, 1800.0f, 2000.0f));

  require(tracker.output().tracks[0].local_track_id == track_id,
          "a reduced-zone teleport must not replace the transiting track");
  require(tracker.output().tracks[0].coasting,
          "a reduced-zone teleport should coast instead of accepting repeated jump samples");
  require(std::fabs(tracker.output().tracks[0].x_mm - accepted_x) < 500.0f,
          "repeated reduced-zone jump samples must not reanchor the displayed coordinate");
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

void replay_log_serializes_reduced_zone_tracker_input() {
  PresenceReplayLog replay_log;
  PresenceTrackerInput tracker_input = make_reduced_position_input(1000, 1200.0f, 1800.0f);
  PresenceReplayRawInput raw_input;
  DiagnosticSnapshot snapshot;
  snapshot.ms = 1000;

  replay_log.update(tracker_input, raw_input, snapshot);

  char line[2048]{};
  require(replay_log.format_ndjson_sample(0, line, sizeof(line)),
          "reduced-zone replay sample should fit in the diagnostics line buffer");
  require(std::string(line).find("\"tg\":[[1,1200,1800,0,2163,1,0]") != std::string::npos,
          "replay tracker input should include the reduced-zone admission bit");
}

void replay_log_serializes_per_target_filter_admission() {
  PresenceReplayLog replay_log;
  PresenceTrackerInput tracker_input = make_position_input(1000, -1200.0f, 1800.0f);
  tracker_input.targets[0].filter_blocked = true;
  PresenceReplayRawInput raw_input;
  DiagnosticSnapshot snapshot;
  snapshot.ms = 1000;

  replay_log.update(tracker_input, raw_input, snapshot);

  char line[2048]{};
  require(replay_log.format_ndjson_sample(0, line, sizeof(line)),
          "per-target filter replay sample should fit in the diagnostics line buffer");
  require(std::string(line).find("\"tg\":[[1,-1200,1800,0,2163,0,1]") != std::string::npos,
          "replay tracker input should preserve the per-target filter admission bit");
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

void static_assist_does_not_hold_a_reduced_zone_coordinate() {
  PresenceFusion fusion;
  PresenceFusionInput input;
  PresenceTrackerOutput tracker_output;
  tracker_output.presence = true;
  tracker_output.confirmed_track_count = 1;
  tracker_output.tracks[0].valid = true;
  tracker_output.tracks[0].x_mm = 600.0f;
  tracker_output.tracks[0].y_mm = 1800.0f;
  input.static_radar.detection_distance_mm = 1900.0f;
  arm_static_assist(fusion, input, tracker_output);

  input.now_ms = 3000;
  input.pir_motion = false;
  tracker_output.tracks[0].reduced_zone = true;
  fusion.update(input, tracker_output);

  input.now_ms = 3500;
  tracker_output = {};
  tracker_output.drop_reason = "lost_without_exit";
  tracker_output.drop_ms = input.now_ms;
  fusion.update(input, tracker_output);

  require(fusion.output().static_assist_active,
          "static radar may still maintain non-spatial presence after reduced-zone loss");
  require(!fusion.output().static_assist_held_target.valid,
          "static assist must not pin the final reduced-zone coordinate");
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

void fixed_state_json_builder_matches_string_contract() {
  DeviceConfigCache cache;
  cache.update(
      R"({"zones":[{"id":"zone_1","name":"Desk","type":"detection","shape":"polygon","points":[[0,0],[1000,0],[1000,1000],[0,1000]]}]})");
  SoftwareZoneEvidence zone_evidence;
  DeviceStateJsonInput input;
  input.now_ms = 1234;
  input.presence = true;
  input.targets[0] = {"target_1", "T1", "#ff6b7a", 420.0f,
                      1800.0f, true, true, 2};
  const std::string expected =
      build_device_state_json(cache, zone_evidence, "{}", input);

  char storage[8192];
  FixedTextBuffer output(storage, sizeof(storage));
  require(build_device_state_json(output, cache, zone_evidence, "{}", input),
          "fixed state JSON builder should fit its embedded workspace");
  require(std::string(output.data(), output.size()) == expected,
          "fixed state JSON builder must preserve the public contract");

  char too_small[64];
  FixedTextBuffer truncated(too_small, sizeof(too_small));
  require(!build_device_state_json(truncated, cache, zone_evidence, "{}",
                                   input),
          "fixed state JSON builder must reject truncated snapshots");
}

void state_json_exposes_bounded_production_fusion_metadata() {
  DeviceConfigCache cache;
  SoftwareZoneEvidence zone_evidence;
  DeviceStateJsonInput input;
  input.fusion_build_enabled = true;
  input.fusion_authoritative = true;
  input.fusion_state = "active";
  input.local_targets[0] = {100.0f, 1200.0f, true};
  input.targets[0] = {"target_1", "T1", "#ff6b7a", 420.0f, 1800.0f, true, true, 2};

  const std::string json = build_device_state_json(cache, zone_evidence, "{}", input);
  require(json.find("\"fusion\":{\"buildEnabled\":true,\"authoritative\":true,\"state\":\"active\"}") !=
              std::string::npos,
          "state JSON should expose compact production fusion status");
  require(json.find("\"active\":true,\"fused\":true,\"sourceCount\":2") != std::string::npos,
          "state JSON should identify a target corroborated by multiple sensors");
  require(json.find(
              "\"local\":{\"presence\":false,\"motion\":false,\"targetCount\":0,\"targets\":[{\"x\":100,\"y\":1200,\"active\":true}") !=
              std::string::npos,
          "state JSON should preserve bounded local radar coordinates beside fusion output");
}

void remote_fusion_accepts_a_verified_leader_reboot_term_reset() {
  FusionAutomationState state;
  SpatialTransform2D transform{};
  transform.valid = true;
  transform.sample_count = 1;
  const auto snapshot = [](const char *boot_id, uint32_t term) {
    FusionSnapshot value{};
    value.occupied = true;
    std::strcpy(value.site_id, "site-a");
    std::strcpy(value.group_id, "group-a");
    std::strcpy(value.leader_node_id, "node-a");
    std::strcpy(value.boot_id, boot_id);
    value.sequence = 1;
    value.group_term = term;
    value.source_sequence = 1;
    value.ttl_ms = FusionSnapshot::DEFAULT_TTL_MS;
    value.target_count = 1;
    value.presence = true;
    value.targets[0].valid = true;
    value.targets[0].fused_track_id = 1;
    value.targets[0].state = SpatialShadowTrackState::CONFIRMED;
    value.targets[0].source_count = 2;
    value.targets[0].confidence = 90;
    return value;
  };

  require(state.update_remote(snapshot("boot-old", 4), "room-a", transform),
          "initial remote fusion authority should be accepted");
  state.mark_unavailable("site-a", "room-a", "group-a", "node-a", 4,
                         false);
  require(state.update_remote(snapshot("boot-new", 2), "room-a", transform,
                              true) &&
              state.snapshot().authoritative &&
              state.snapshot().group_term == 2 &&
              std::strcmp(state.snapshot().boot_id, "boot-new") == 0,
          "an admitted rebooted leader must recover after its runtime term reset");
}

void state_json_exposes_complete_automation_snapshot_without_false_clear() {
  DeviceConfigCache cache;
  cache.update(
      R"({"zones":[{"id":"zone_1","name":"Desk","type":"detection","shape":"polygon","points":[[0,0],[1000,0],[1000,1000],[0,1000]]}]})");
  SoftwareZoneEvidence zone_evidence;
  zone_evidence.state_zone_counts[0] = 1;
  DeviceStateJsonInput input;
  input.now_ms = 3000;
  input.local_presence = false;
  input.local_motion = false;
  input.local_target_count = 0;

  FusionAutomationState automation_state;
  SpatialProductionFusionOutput fusion{};
  fusion.build_enabled = true;
  fusion.authoritative = true;
  fusion.state = SpatialProductionFusionState::ACTIVE;
  fusion.source_sequence = 9;
  fusion.target_count = 1;
  fusion.targets[0].valid = true;
  fusion.targets[0].fused_track_id = 1;
  fusion.targets[0].state = SpatialShadowTrackState::CONFIRMED;
  fusion.targets[0].source_count = 2;
  fusion.targets[0].confidence = 90;
  fusion.targets[0].x_mm = 500.0f;
  fusion.targets[0].y_mm = -500.0f;
  require(automation_state.update_local("site-a", "room-a", "group-a",
                                        "node-a", "boot-a", 3, fusion, 2500),
          "automation fixture should become healthy");
  input.automation = &automation_state.snapshot();
  esphome::radar_api_server::FloorplanRadarPose pose;
  pose.valid = true;
  pose.origin_x_px = 100.0f;
  pose.origin_y_px = 100.0f;
  pose.mm_per_px_x = 10.0f;
  pose.mm_per_px_y = 10.0f;
  pose.pose_version = 3;
  const std::string floorplan = R"({
    "floorplanId":"fp-0123456789abcdef0123456789abcdef",
    "automationAreas":{"schemaVersion":1,"revision":2,"areas":[
      {"areaId":"desk","name":"Desk","spaceId":"room-a","enabled":true,
       "pointsPx":[[100,100],[200,100],[200,200],[100,200]]}
    ]}
  })";
  esphome::radar_api_server::SharedAutomationAreaCache shared_areas;
  require(shared_areas.update(
              floorplan.data(), floorplan.size(),
              "fp-0123456789abcdef0123456789abcdef", "room-a", 1234,
              pose),
          "shared area fixture should parse");
  input.shared_automation_areas = &shared_areas;
  const std::string healthy =
      build_device_state_json(cache, zone_evidence, "{}", input);
  require(healthy.find(
              "\"automation\":{\"schemaVersion\":1,\"complete\":true,\"health\":\"healthy\",\"available\":true") !=
              std::string::npos,
          "state JSON should expose a complete healthy automation snapshot");
  require(healthy.find("\"spaceId\":\"room-a\"") != std::string::npos &&
              healthy.find("\"sequence\":9") != std::string::npos,
          "automation snapshot should preserve stable identity and sequence");
  require(healthy.find(
              "\"zoneId\":\"zone_1\",\"name\":\"Desk\",\"type\":\"detection\",\"presence\":true") !=
              std::string::npos,
          "automation snapshot should include complete Zone state");
  require(healthy.find(
              "\"floorplanId\":\"fp-0123456789abcdef0123456789abcdef\",\"areasRevision\":2,\"areasAvailable\":true,\"areas\":[{\"areaId\":\"desk\",\"name\":\"Desk\",\"presence\":true,\"targetCount\":1}]") !=
              std::string::npos,
          "automation snapshot should expose canonical Floorplan area state");

  automation_state.mark_unavailable("site-a", "room-a", "group-a",
                                    "node-a", 3, false);
  input.automation = &automation_state.snapshot();
  const std::string unavailable =
      build_device_state_json(cache, zone_evidence, "{}", input);
  require(unavailable.find(
              "\"health\":\"unavailable\",\"available\":false") !=
              std::string::npos,
          "transport loss should expose unavailable health");
  require(unavailable.find(
              "\"presence\":null,\"motion\":null,\"targetCount\":null") !=
              std::string::npos,
          "transport loss must not serialize a false unoccupied transition");
  require(unavailable.find(
              "\"zoneId\":\"zone_1\",\"name\":\"Desk\",\"type\":\"detection\",\"presence\":null,\"targetCount\":null") !=
              std::string::npos,
          "unavailable automation must not falsely clear Zone occupancy");
  require(unavailable.find(
              "\"areasAvailable\":false,\"areas\":[{\"areaId\":\"desk\",\"name\":\"Desk\",\"presence\":null,\"targetCount\":null}]") !=
              std::string::npos,
          "unavailable automation must not falsely clear canonical areas");
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

void target_admission_blocks_only_the_filtered_target() {
  const std::array<PresenceTargetAdmissionInput, 3> inputs{{
      {true, true, true},
      {true, false, true},
      {false, false, false},
  }};
  const auto result = evaluate_presence_target_admission(inputs);

  require(result.observed_count == 2, "admission should count both observed targets");
  require(result.blocked_count == 1, "admission should count only the filtered target");
  require(result.admitted_count == 1, "the unfiltered target should remain admitted");
  require(!result.admitted[0] && result.admitted[1] && !result.admitted[2],
          "admission must be decided independently for each target");
  require(result.association_candidates[0] && result.association_candidates[1] &&
              !result.association_candidates[2],
          "filtered observations should remain available only for track association");
  require(!result.fully_filter_blocked,
          "one filtered target must not globally block another admitted target");

  const std::array<PresenceTargetAdmissionInput, 3> all_blocked{{
      {true, true, true},
      {true, true, true},
      {false, false, false},
  }};
  const auto blocked_result = evaluate_presence_target_admission(all_blocked);
  require(blocked_result.fully_filter_blocked,
          "the frame-level filter guard should remain active when every observation is blocked");
}

void filtered_false_positive_does_not_prevent_real_target_admission() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, -1200.0f, 2000.0f));
  tracker.update(make_position_input(1000, -1200.0f, 2000.0f));
  tracker.update(make_position_input(1500, -1200.0f, 2000.0f));

  for (uint32_t now_ms = 2000; now_ms <= 3000; now_ms += 500) {
    const std::array<PresenceTargetAdmissionInput, 3> admission_inputs{{
        {true, true, true},
        {true, false, true},
        {false, false, false},
    }};
    const auto admission = evaluate_presence_target_admission(admission_inputs);
    PresenceTrackerInput input = make_input(now_ms);
    input.filter_blocked = admission.fully_filter_blocked;
    if (admission.association_candidates[0]) {
      set_target(input, 0, -1200.0f, 2000.0f);
      input.targets[0].filter_blocked = true;
    }
    if (admission.association_candidates[1])
      set_target(input, 1, 1200.0f, 2000.0f);
    tracker.update(input);
  }

  const auto &output = tracker.output();
  require(output.input_detection_count == 1,
          "only the real target should reach tracker admission");
  require(output.confirmed_track_count == 1,
          "the real target should confirm while the calibrated false positive is filtered");
  bool real_target_found = false;
  for (const auto &track : output.tracks) {
    if (track.valid && std::strcmp(track.state, "confirmed") == 0 && track.x_mm > 0.0f)
      real_target_found = true;
  }
  require(real_target_found,
          "the admitted target should keep its own coordinates instead of being globally suppressed");
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
  confirm_two_tracks(tracker, -1000.0f, 2000.0f, 1000.0f, 2000.0f);
  require(tracker.output().confirmed_track_count == 2, "two stable detections should confirm two tracks");

  tracker.update(make_two_target_input(3000, 1000.0f, 2000.0f, -1000.0f, 2000.0f));
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
  confirm_two_tracks(tracker, -1000.0f, 2000.0f, 1000.0f, 2000.0f);

  PresenceTrackerInput input = make_two_target_input(3000, -1000.0f, 2000.0f, 1000.0f, 2000.0f);
  set_target(input, 2, 0.0f, 5000.0f);
  tracker.update(input);
  require(tracker.output().confirmed_track_count == 2, "existing stable tracks should remain confirmed");
  require(tracker.output().tentative_track_count == 1, "unmatched detection should allocate the free track slot");
  require(tracker.output().active_track_count == 3, "all three hardware target slots should be represented");
}

void unexpected_jump_requires_three_consistent_measurements() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  tracker.update(make_position_input(2000, 1600.0f, 2000.0f));
  const auto candidate = require_track(tracker.output(), 0);
  require_near(candidate.x_mm, 0.0f, 0.01f,
               "a single teleport measurement must not move the displayed track");
  require(std::strcmp(candidate.state, "coasting") == 0,
          "a single teleport measurement should keep the prior track in coasting");
  require(std::strcmp(candidate.reason, "coasting_teleport_candidate") == 0,
          "the rejected jump should remain diagnosable");

  tracker.update(make_position_input(2500, 1620.0f, 2010.0f));
  const auto second_candidate = require_track(tracker.output(), 0);
  require_near(second_candidate.x_mm, 0.0f, 0.01f,
               "two jump measurements must still protect the established track");
  require(std::strcmp(second_candidate.state, "coasting") == 0,
          "two jump measurements should remain a guarded candidate");

  tracker.update(make_position_input(3000, 1600.0f, 1990.0f));
  const auto accepted = require_track(tracker.output(), 0);
  require_near(accepted.x_mm, 1600.0f, 0.01f,
               "three consistent jump measurements should reacquire at the latest position");
  require_near(accepted.y_mm, 1990.0f, 0.01f,
               "reacquisition should use the latest confirmed measurement");
  require_near(accepted.vx_mm_s, 0.0f, 0.01f,
               "confirmed teleport should reset x velocity");
  require_near(accepted.vy_mm_s, 0.0f, 0.01f,
               "confirmed teleport should reset y velocity");
}

void ambiguous_distant_candidates_coast_without_splitting() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t established_id = require_track(tracker.output(), 0).local_track_id;

  tracker.update(make_two_target_input(2000, -500.0f, 2000.0f,
                                       500.0f, 2000.0f));
  const auto &output = tracker.output();
  require(output.active_track_count == 1,
          "equally plausible distant measurements must not split one person");
  require(output.coasting_track_count == 1 &&
              output.tentative_track_count == 0,
          "an ambiguous association should coast instead of allocating candidates");
  require(require_track(output, 0).local_track_id == established_id,
          "ambiguity must preserve the established identity");
}

void pir_does_not_fast_confirm_an_additional_target() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  for (uint32_t now_ms = 2000; now_ms <= 3500; now_ms += 500) {
    auto input = make_two_target_input(now_ms, 0.0f, 2000.0f,
                                       1800.0f, 3000.0f);
    input.pir_motion = true;
    tracker.update(input);
  }
  require(tracker.output().confirmed_track_count == 1 &&
              tracker.output().tentative_track_count == 1,
          "PIR must not fast-confirm a possible reflection beside an established person");

  for (uint32_t now_ms = 4000; now_ms <= 5500; now_ms += 500) {
    auto moving = make_two_target_input(now_ms, 0.0f, 2000.0f,
                                        2500.0f + (now_ms - 4000) * 0.2f,
                                        3000.0f);
    moving.pir_motion = true;
    tracker.update(moving);
  }
  require(tracker.output().confirmed_track_count == 2,
          "an independently moving second person must still confirm without waiting for the guard timeout");
}

void cold_start_two_slot_reflection_keeps_one_guarded_candidate() {
  PresenceTracker tracker;
  for (uint32_t now_ms = 500; now_ms <= 2500; now_ms += 500) {
    auto input = make_two_target_input(now_ms, 0.0f, 2000.0f,
                                       900.0f, 3000.0f);
    input.pir_motion = true;
    tracker.update(input);
  }
  require(tracker.output().confirmed_track_count == 1 &&
              tracker.output().tentative_track_count == 1,
          "a two-slot cold start must not immediately publish a reflection as a second person");
}

void captured_reduced_reflection_does_not_become_a_second_person() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 546.0f, 2021.0f));
  tracker.update(make_position_input(1000, 657.0f, 2178.0f));
  tracker.update(make_position_input(1500, 416.0f, 1871.0f));

  const std::array<std::array<float, 4>, 12> captured{{
      {{416.0f, 1777.0f, -772.0f, 3249.0f}},
      {{345.0f, 1695.0f, -667.0f, 3175.0f}},
      {{280.0f, 1681.0f, -677.0f, 3163.0f}},
      {{233.0f, 1690.0f, -798.0f, 3094.0f}},
      {{194.0f, 1663.0f, -722.0f, 3022.0f}},
      {{171.0f, 1628.0f, -620.0f, 2976.0f}},
      {{177.0f, 1625.0f, -566.0f, 3001.0f}},
      {{171.0f, 1628.0f, -566.0f, 2976.0f}},
      {{194.0f, 1690.0f, -620.0f, 3022.0f}},
      {{233.0f, 1690.0f, -722.0f, 3022.0f}},
      {{280.0f, 1681.0f, -677.0f, 3163.0f}},
      {{345.0f, 1695.0f, -667.0f, 3175.0f}},
  }};

  uint32_t now_ms = 2000;
  for (std::size_t index = 0; index < captured.size(); ++index) {
    const auto &point = captured[index];
    auto input = make_two_target_input(now_ms, point[0], point[1],
                                       point[2], point[3]);
    input.pir_motion = true;
    if (index >= 3)
      input.targets[1].reduced_zone = true;
    tracker.update(input);
    require(tracker.output().confirmed_track_count == 1,
            "the captured one-person reduced-zone reflection must not confirm as a second person");
    now_ms += 500;
  }
}

void secondary_coasting_expires_while_its_parent_remains_observed() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  for (uint32_t now_ms = 2000; now_ms <= 5500; now_ms += 500) {
    auto input = make_two_target_input(now_ms, 0.0f, 2000.0f,
                                       1800.0f + (now_ms - 2000) * 0.4f,
                                       3000.0f);
    tracker.update(input);
  }
  require(tracker.output().confirmed_track_count == 2,
          "the fixture must first confirm an independently moving second target");

  for (uint32_t now_ms = 6000; now_ms <= 9000; now_ms += 500)
    tracker.update(make_position_input(now_ms, 0.0f, 2000.0f));
  require(tracker.output().confirmed_track_count == 1 &&
              tracker.output().coasting_track_count == 0,
          "a vanished secondary track must not coast for the full primary-person hold");
}

void kalman_filter_smooths_measurement_and_holds_coasting_position() {
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
  const auto coasting = require_track(tracker.output(), 0);
  require_near(coasting.x_mm, updated.x_mm, 0.01f,
               "coasting must hold the last measured x position instead of extrapolating it");
  require_near(coasting.y_mm, updated.y_mm, 0.01f,
               "coasting must hold the last measured y position instead of extrapolating it");
  require(std::isfinite(coasting.vx_mm_s) && std::isfinite(coasting.vy_mm_s),
          "coasting should preserve finite velocity metadata for reassociation");

  tracker.update(make_input(100500));
  const auto repeated_coasting = require_track(tracker.output(), 0);
  require_near(repeated_coasting.x_mm, updated.x_mm, 0.01f,
               "repeated missed frames must not accumulate velocity into the displayed x position");
  require_near(repeated_coasting.y_mm, updated.y_mm, 0.01f,
               "repeated missed frames must not accumulate velocity into the displayed y position");
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

void local_track_id_survives_coasting_and_reacquisition() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  const auto &confirmed = require_track(tracker.output(), 0);
  const uint32_t confirmed_id = confirmed.local_track_id;
  const float confirmed_variance_x = confirmed.position_variance_x_mm2;
  const float confirmed_variance_y = confirmed.position_variance_y_mm2;
  require(confirmed_id != 0, "a confirmed track must receive a non-zero local ID");
  require(confirmed.sample_age_ms == 0, "a freshly measured confirmed track must have zero sample age");
  require(std::isfinite(confirmed_variance_x) && confirmed_variance_x >= 0.0f &&
              std::isfinite(confirmed_variance_y) && confirmed_variance_y >= 0.0f,
          "confirmed tracks must expose finite non-negative position variance");

  tracker.update(make_input(2000));
  const auto &coasting = require_track(tracker.output(), 0);
  require(coasting.coasting, "a missed confirmed track should enter coasting");
  require(coasting.local_track_id == confirmed_id, "coasting must preserve the local track ID");
  require(coasting.sample_age_ms == 500, "one missed device tick must expose a 500 ms sample age");
  require(coasting.position_variance_x_mm2 > confirmed_variance_x &&
              coasting.position_variance_y_mm2 > confirmed_variance_y,
          "prediction without a measurement must increase positional uncertainty");
  const float coasting_variance_x = coasting.position_variance_x_mm2;
  const float coasting_variance_y = coasting.position_variance_y_mm2;

  tracker.update(make_position_input(2500, 100.0f, 2000.0f));
  const auto &reacquired = require_track(tracker.output(), 0);
  require(!reacquired.coasting, "a nearby detection should reacquire the coasting track");
  require(reacquired.local_track_id == confirmed_id, "reacquisition must preserve the local track ID");
  require(reacquired.sample_age_ms == 0, "reacquisition must reset sample age to zero");
  require(reacquired.position_variance_x_mm2 < coasting_variance_x &&
              reacquired.position_variance_y_mm2 < coasting_variance_y,
          "a new measurement must reduce coasting positional uncertainty");
}

void tracker_time_metadata_survives_millis_wrap() {
  PresenceTracker tracker;
  constexpr uint32_t first_ms = std::numeric_limits<uint32_t>::max() - 1000U;
  constexpr uint32_t second_ms = std::numeric_limits<uint32_t>::max() - 500U;
  constexpr uint32_t third_ms = std::numeric_limits<uint32_t>::max();

  tracker.update(make_target_input(first_ms, true, false, 1200.0f, 1));
  tracker.update(make_target_input(second_ms, true, false, 1200.0f, 1));
  tracker.update(make_target_input(third_ms, true, false, 1200.0f, 1));
  const uint32_t local_track_id = require_track(tracker.output(), 0).local_track_id;

  tracker.update(make_input(499));
  const auto &wrapped = require_track(tracker.output(), 0);
  require(wrapped.coasting, "a track must keep coasting across the 32-bit millis wrap");
  require(wrapped.local_track_id == local_track_id, "millis wrap must not replace the local track ID");
  require(wrapped.sample_age_ms == 500, "sample age must use wrap-safe unsigned elapsed time");
  require(wrapped.exit_recent && wrapped.exit_age_ms == 500,
          "recent exit evidence must remain valid across millis wrap");

  tracker.update(make_target_input(999, true, false, 1200.0f, 1));
  const auto &reacquired = require_track(tracker.output(), 0);
  require(!reacquired.coasting && reacquired.local_track_id == local_track_id,
          "a wrapped timestamp must still allow normal track reacquisition");
  require(reacquired.sample_age_ms == 0, "post-wrap reacquisition must reset sample age");
}

void local_track_ids_survive_sensor_slot_reordering() {
  PresenceTracker tracker;
  confirm_two_tracks(tracker, -1000.0f, 2000.0f, 1000.0f, 2000.0f);

  uint32_t left_id = 0;
  uint32_t right_id = 0;
  for (const auto &track : tracker.output().tracks) {
    if (!track.valid)
      continue;
    if (track.x_mm < 0.0f)
      left_id = track.local_track_id;
    else
      right_id = track.local_track_id;
  }
  require(left_id != 0 && right_id != 0 && left_id != right_id,
          "two confirmed spatial tracks must receive distinct local IDs");

  tracker.update(make_two_target_input(3000, 1000.0f, 2000.0f, -1000.0f, 2000.0f));
  for (const auto &track : tracker.output().tracks) {
    if (!track.valid)
      continue;
    if (track.x_mm < 0.0f)
      require(track.local_track_id == left_id,
              "the left target ID must survive LD2450 sensor-slot reordering");
    else
      require(track.local_track_id == right_id,
              "the right target ID must survive LD2450 sensor-slot reordering");
  }
}

void expired_track_gets_a_new_local_track_id() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));
  const uint32_t previous_id = require_track(tracker.output(), 0).local_track_id;

  miss_frames(tracker, 25, 2000);
  require(tracker.output().active_track_count == 0, "a fully expired track must release its slot");

  tracker.update(make_position_input(15000, 0.0f, 2000.0f));
  tracker.update(make_position_input(15500, 0.0f, 2000.0f));
  tracker.update(make_position_input(16000, 0.0f, 2000.0f));
  const uint32_t replacement_id = require_track(tracker.output(), 0).local_track_id;
  require(replacement_id != 0 && replacement_id != previous_id,
          "a target created after full expiry must receive a new local track ID");
}

void tentative_track_is_not_published_as_a_local_observation() {
  PresenceTracker tracker;
  PresenceFusion fusion;
  LocalObservationAdapter adapter;

  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = 500;
  fusion.update(fusion_input, tracker.output());
  adapter.update(500, false, false, tracker.output(), fusion.output());

  require(tracker.output().tentative_track_count == 1, "the device scenario must contain a tentative track");
  require(adapter.output().track_count == 0, "tentative detections must not leave the local node");
}

void local_observation_copies_tracks_and_optional_evidence() {
  PresenceTracker tracker;
  PresenceFusion fusion;
  LocalObservationAdapter adapter;

  tracker.update(make_target_input(500, true, false, 1200.0f));
  tracker.update(make_target_input(1000, true, false, 1200.0f));
  tracker.update(make_target_input(1500, true, false, 1200.0f));

  PresenceFusionInput fusion_input;
  fusion_input.now_ms = 1500;
  fusion_input.pir_motion = true;
  fusion_input.static_radar.available = true;
  fusion_input.static_radar.presence = true;
  fusion_input.static_radar.still = true;
  fusion_input.static_radar.detection_distance_mm = 2300.0f;
  fusion.update(fusion_input, tracker.output());
  adapter.update(1500, true, false, tracker.output(), fusion.output());

  const auto &observation = adapter.output();
  require(observation.version == LocalObservation::VERSION, "local observation version must be explicit");
  require(observation.sequence == 1, "the first local observation must start at sequence one");
  require(observation.observed_at_ms == 1500, "the observation must retain its local sample time");
  require(observation.ttl_ms == LocalObservation::DEFAULT_TTL_MS, "the observation TTL must be bounded");
  require(observation.track_count == 1, "one confirmed device track must produce one observation track");
  require(observation.tracks[0].local_track_id == require_track(tracker.output(), 0).local_track_id,
          "the observation must carry the tracker's stable local ID");
  require(observation.tracks[0].sample_age_ms == 0,
          "the observation must carry the source measurement age");
  require_near(observation.tracks[0].position_variance_x_mm2,
               require_track(tracker.output(), 0).position_variance_x_mm2, 0.01f,
               "the observation must carry tracker x uncertainty");
  require_near(observation.tracks[0].position_variance_y_mm2,
               require_track(tracker.output(), 0).position_variance_y_mm2, 0.01f,
               "the observation must carry tracker y uncertainty");
  require(observation.tracks[0].room_state == LocalObservationRoomState::INSIDE,
          "room evidence must be normalized without losing the tracker result");
  require(observation.evidence.pir_motion, "PIR evidence must be represented independently");
  require(observation.evidence.tracker_presence, "tracker presence must be represented independently");
  require(observation.evidence.static_radar_available && observation.evidence.static_radar_presence,
          "available static-radar evidence must be copied into the observation");
  require_near(observation.evidence.static_radar_distance_mm, 2300.0f, 0.01f,
               "the observation must use fusion's canonical static-radar distance");
}

void local_observation_remains_bounded_with_three_targets() {
  PresenceTracker tracker;
  for (uint32_t now_ms = 500; now_ms <= 2500; now_ms += 500) {
    PresenceTrackerInput input = make_input(now_ms);
    if (now_ms == 500) {
      set_target(input, 0, -600.0f, 2000.0f);
      set_target(input, 1, 0.0f, 1750.0f);
      set_target(input, 2, 600.0f, 2000.0f);
    } else {
      set_target(input, 0, -1000.0f, 2000.0f);
      set_target(input, 1, 0.0f, 2500.0f);
      set_target(input, 2, 1000.0f, 2000.0f);
    }
    tracker.update(input);
  }

  PresenceFusion fusion;
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = 2500;
  fusion.update(fusion_input, tracker.output());
  LocalObservationAdapter adapter;
  adapter.update(2500, false, false, tracker.output(), fusion.output());

  require(tracker.output().confirmed_track_count == 3,
          "the device scenario must contain all three LD2450 targets");
  require(adapter.output().track_count == LocalObservation::MAX_TRACKS,
          "the local observation must publish exactly the bounded three-track capacity");
  require(adapter.output().tracks[0].local_track_id != adapter.output().tracks[1].local_track_id &&
              adapter.output().tracks[0].local_track_id != adapter.output().tracks[2].local_track_id &&
              adapter.output().tracks[1].local_track_id != adapter.output().tracks[2].local_track_id,
          "each published target must retain a distinct local identity");
}

void local_observation_handles_missing_static_radar_and_filter_block() {
  PresenceTrackerOutput tracker_output;
  PresenceFusion fusion;
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = 500;
  fusion_input.filter_blocked = true;
  fusion.update(fusion_input, tracker_output);

  LocalObservationAdapter adapter;
  adapter.update(500, false, true, tracker_output, fusion.output());
  const auto &observation = adapter.output();
  require(observation.track_count == 0, "missing hardware must not invent a spatial target");
  require(observation.evidence.filter_blocked, "filter state must be carried as evidence");
  require(!observation.evidence.static_radar_available,
          "an absent LD2410C must remain optional and unavailable");
  require(!observation.evidence.static_radar_presence,
          "an absent LD2410C must not create presence evidence");
}

void local_observation_is_shadow_only_and_sequence_advances() {
  PresenceTracker tracker;
  tracker.update(make_position_input(500, 0.0f, 2000.0f));
  tracker.update(make_position_input(1000, 0.0f, 2000.0f));
  tracker.update(make_position_input(1500, 0.0f, 2000.0f));

  PresenceFusion fusion;
  PresenceFusionInput fusion_input;
  fusion_input.now_ms = 1500;
  fusion.update(fusion_input, tracker.output());

  const bool tracker_presence_before = tracker.output().presence;
  const bool tracker_motion_before = tracker.output().motion;
  const bool fusion_presence_before = fusion.output().presence;
  const char *fusion_reason_before = fusion.output().presence_reason;

  DeviceConfigCache config_cache;
  SoftwareZoneEvidence zone_evidence;
  DeviceStateJsonInput state_input;
  state_input.now_ms = 1500;
  state_input.presence = fusion.output().presence;
  state_input.motion = fusion.output().motion;
  state_input.presence_reason = fusion.output().presence_reason;
  state_input.presence_off_reason = fusion.output().presence_off_reason;
  state_input.motion_reason = fusion.output().motion_reason;
  const std::string public_state_before =
      build_device_state_json(config_cache, zone_evidence, tracker.debug_json(), state_input);

  LocalObservationAdapter adapter;
  adapter.update(1500, false, false, tracker.output(), fusion.output());
  const uint32_t first_sequence = adapter.output().sequence;
  adapter.update(2000, false, false, tracker.output(), fusion.output());
  const std::string public_state_after =
      build_device_state_json(config_cache, zone_evidence, tracker.debug_json(), state_input);

  require(adapter.output().sequence == first_sequence + 1,
          "every local snapshot must advance its monotonic per-boot sequence");
  require(tracker.output().presence == tracker_presence_before && tracker.output().motion == tracker_motion_before,
          "building a local observation must not mutate tracker decisions");
  require(fusion.output().presence == fusion_presence_before &&
              std::strcmp(fusion.output().presence_reason, fusion_reason_before) == 0,
          "building a local observation must not mutate final presence decisions");
  require(public_state_after == public_state_before,
          "building local observations must leave the standalone public state byte-for-byte unchanged");
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

void device_config_cache_tracks_the_local_floorplan_pose_version() {
  DeviceConfigCache cache;
  cache.update(
      R"({"version":1,"floorplan":{"radar":{"originPx":[100,120],"rotationDeg":15,"poseVersion":23}}})");
  require(cache.floorplan_pose_version() == 23,
          "device config must expose the persisted local radar pose version");
  cache.update(
      R"({"version":1,"floorplan":{"radar":{"originX":100,"originY":120,"rotation":15,"scale":1}}})");
  require(cache.floorplan_pose_version() == 1,
          "legacy local radar placement must migrate to the initial pose version");
  cache.update(
      R"({"version":1,"floorplan":{"scaleMmPerPx":10.5,"radar":{"originX":100,"originY":120,"rotation":15,"scale":1,"poseVersion":23}}})");
  const auto &pose = cache.floorplan_radar_pose();
  const auto transform = pose.spatial_transform();
  require(pose.valid && pose.pose_version == 23 && pose.scale_factor == 1.0f &&
              transform.valid && transform.origin_x_mm == 1050 &&
              transform.origin_y_mm == 1260 &&
              transform.rotation_cdeg == 1500,
          "device config must cache a bounded world transform for local shadow fusion");
  cache.update(R"({"version":1,"floorplan":{"radar":{}}})");
  require(cache.floorplan_pose_version() == 0,
          "an empty radar placement must not become a valid pose");
  require(!cache.floorplan_radar_pose().valid,
          "an incomplete local placement must not expose a spatial transform");
  cache.update(R"({"version":1,"floorplan":{"radar":{"poseVersion":4294967296}}})");
  require(cache.floorplan_pose_version() == 0,
          "an overflowing pose version must fail closed");
  cache.clear();
  require(cache.floorplan_pose_version() == 0,
          "clearing device config must clear the cached pose version");
}

void direct_upgrade_hydrates_floorplan_scale_without_moving_the_radar() {
  const std::string legacy_device_config =
      R"({"version":1,"floorplan":{"enabled":true,"hasImage":true,"radar":{"originX":220.14,"originY":241.53,"rotation":173.2,"scale":1,"poseVersion":6}}})";
  const std::string stored_floorplan =
      R"({"version":2,"floorplanId":"fp-direct-upgrade","geometryVersion":3,"scale":{"widthMm":3900,"heightMm":6000,"outerBoundsPx":[0,0,384,656],"mmPerPxX":10.16,"mmPerPxY":9.15},"radar":{"originPx":[220.14,241.53],"rotationDeg":173.2,"scale":1,"poseVersion":6},"rooms":[]})";

  DeviceConfigCache cache;
  cache.update(legacy_device_config);
  require(!cache.floorplan_radar_pose().valid,
          "a direct-upgrade sidecar without scale must fail closed before hydration");
  require(cache.hydrate_floorplan_scale_from_document(stored_floorplan),
          "the stored canonical floorplan must hydrate a missing legacy scale");
  const auto hydrated = cache.floorplan_radar_pose().spatial_transform();
  require(cache.floorplan_scale_hydrated() && hydrated.valid,
          "hydrated direct-upgrade placement must become a valid transform");
  require(hydrated.origin_x_mm == 2237 && hydrated.origin_y_mm == 2210 &&
              hydrated.rotation_cdeg == 17320,
          "migration must preserve the local pose and apply independent X/Y scale");

  cache.clear();
  cache.update(legacy_device_config);
  require(cache.hydrate_floorplan_scale_from_document(stored_floorplan),
          "the same migration must be deterministic after a simulated reboot");
  const auto rebooted = cache.floorplan_radar_pose().spatial_transform();
  require(rebooted.valid &&
              rebooted.origin_x_mm == hydrated.origin_x_mm &&
              rebooted.origin_y_mm == hydrated.origin_y_mm &&
              rebooted.rotation_cdeg == hydrated.rotation_cdeg,
          "reboot hydration must reproduce the same canonical transform");

  cache.update(
      R"({"version":1,"floorplan":{"scaleMmPerPx":10.5,"scaleMmPerPxX":10.16,"scaleMmPerPxY":9.15,"radar":{"originX":220.14,"originY":241.53,"rotation":173.2,"scale":1,"poseVersion":6}}})");
  const auto current = cache.floorplan_radar_pose().spatial_transform();
  require(current.valid && !cache.floorplan_scale_hydrated() &&
              current.origin_x_mm == hydrated.origin_x_mm &&
              current.origin_y_mm == hydrated.origin_y_mm,
          "current sidecars must use persisted axis scale without migration");
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

void zone_policy_and_evidence_share_cached_geometry() {
  DeviceConfigCache cache;
  cache.update(
      R"({"version":1,"zones":[{"id":"zone_1","type":"detection","points":[[-1000,1000],[1000,1000],[1000,3000],[-1000,3000]]},{"id":"zone_2","type":"filter","points":[[1500,1000],[2500,1000],[2500,3000],[1500,3000]]},{"id":"zone_3","type":"exit","points":[[3500,1000],[4500,1000],[4500,3000],[3500,3000]]},{"id":"zone_4","type":"reduced","points":[[2600,1000],[3400,1000],[3400,3000],[2600,3000]]}],"calibrationZones":[{"id":"calibration_1","type":"filter","points":[[-500,1500],[500,1500],[500,2500],[-500,2500]]},{"id":"calibration_2","type":"reduced","points":[[1500,1000],[3500,1000],[3500,3000],[1500,3000]]}]})");

  require(cache.software_zone(1).valid && cache.software_zone(1).contains(0.0f, 2000.0f),
          "device config should compile software-zone geometry once");
  require(cache.calibration_zone(2).valid && cache.calibration_zone(2).contains(3000.0f, 2000.0f),
          "device config should compile calibration-zone geometry through the same path");

  const auto calibration_filter = compute_target_zone_policy(cache, 0.0f, 2000.0f);
  require(calibration_filter.action == ZonePolicyAction::BLOCKED &&
              calibration_filter.source == ZonePolicySource::CALIBRATION &&
              calibration_filter.zone_index == 1 &&
              calibration_filter.diagnostic_mode() == 11,
          "calibration filter should preserve its blocking source and diagnostic code");

  const auto overlapping_filter = compute_target_zone_policy(cache, 2000.0f, 2000.0f);
  require(overlapping_filter.action == ZonePolicyAction::BLOCKED &&
              overlapping_filter.source == ZonePolicySource::SOFTWARE &&
              overlapping_filter.zone_index == 2 &&
              overlapping_filter.diagnostic_mode() == 2,
          "software filter should take precedence over an overlapping reduced zone");

  const auto software_reduced = compute_target_zone_policy(cache, 3000.0f, 2000.0f);
  require(software_reduced.action == ZonePolicyAction::REDUCED &&
              software_reduced.source == ZonePolicySource::SOFTWARE &&
              software_reduced.zone_index == 4 &&
              software_reduced.diagnostic_mode() == 3,
          "software reduced zone should use the shared tentative-admission behavior");

  const auto calibration_reduced = compute_target_zone_policy(cache, 3450.0f, 2000.0f);
  require(calibration_reduced.action == ZonePolicyAction::REDUCED &&
              calibration_reduced.source == ZonePolicySource::CALIBRATION &&
              calibration_reduced.zone_index == 2 &&
              calibration_reduced.diagnostic_mode() == 22,
          "calibration reduced zone should apply outside software reduced zones");

  const auto normal = compute_target_zone_policy(cache, 4800.0f, 2000.0f);
  require(!normal.filtered() && normal.diagnostic_mode() == 0,
          "a target outside every policy zone should remain unfiltered");

  const SoftwareZoneTarget targets[3] = {
      {true, 0.0f, 2000.0f},
      {true, 4000.0f, 2000.0f},
      {false, 0.0f, 0.0f},
  };
  uint32_t exit_last_seen_ms = 0;
  const auto evidence =
      compute_software_zone_evidence(cache, 1000, targets, &exit_last_seen_ms);
  require(evidence.detection_zone_count == 1 &&
              evidence.detection_zone_counts[0] == 1,
          "software evidence should use the cached detection geometry");
  require(evidence.exit_zone_count == 1 && evidence.target_exit_masks[1] == 4,
          "software evidence should use the cached exit geometry");
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

void static_radar_factory_reset_request_is_single_shot_and_cancels_tuning() {
  using esphome::radar_api_server::ControlState;
  using esphome::radar_api_server::StaticRadarFactoryResetRequestResult;
  using esphome::radar_api_server::StaticRadarGateOperationState;

  ControlState state{};
  require(state.request_static_radar_factory_reset() ==
              StaticRadarFactoryResetRequestResult::UNAVAILABLE,
          "factory reset should reject unavailable LD2410C hardware");

  state.static_radar_available = true;
  state.static_radar_tuning_active = true;
  state.static_radar_tuning_requested_active = true;
  state.static_radar_tuning_keepalive_ms = 1234;
  state.pending_static_radar_tuning_session = true;
  state.static_radar_gate_operation_state = StaticRadarGateOperationState::PENDING;
  require(state.request_static_radar_factory_reset() ==
              StaticRadarFactoryResetRequestResult::ACCEPTED,
          "factory reset should be accepted for available LD2410C hardware");
  require(!state.static_radar_tuning_active &&
              !state.static_radar_tuning_requested_active &&
              state.static_radar_tuning_keepalive_ms == 0 &&
              !state.pending_static_radar_tuning_session &&
              state.static_radar_gate_operation_state == StaticRadarGateOperationState::IDLE,
          "factory reset should cancel tuning state and pending gate writes");
  require(state.request_static_radar_factory_reset() ==
              StaticRadarFactoryResetRequestResult::ALREADY_PENDING,
          "factory reset should reject duplicate pending requests");
  require(state.take_static_radar_factory_reset_request(),
          "factory reset should be consumed exactly once");
  require(!state.take_static_radar_factory_reset_request(),
          "consumed factory reset should not run twice");
}

void static_radar_gate_write_is_serialized_and_read_back() {
  using esphome::radar_api_server::ControlState;
  using esphome::radar_api_server::StaticRadarGateOperationState;
  using esphome::radar_api_server::StaticRadarGateRequestResult;

  ControlState state{};
  uint32_t operation_id = 0;
  require(state.request_static_radar_gate(3, 72, &operation_id) == StaticRadarGateRequestResult::ACCEPTED &&
              operation_id == 1,
          "first LD2410C gate write should be accepted");
  uint32_t blocked_operation_id = 0;
  require(state.request_static_radar_gate(4, 60, &blocked_operation_id) == StaticRadarGateRequestResult::BUSY,
          "a second LD2410C gate write must not overwrite the in-flight request");

  uint8_t gate = 0;
  uint8_t sensitivity = 0;
  require(state.take_static_radar_gate_request(&gate, &sensitivity) && gate == 3 && sensitivity == 72,
          "the accepted gate write should be consumed unchanged");
  require(!state.take_static_radar_gate_request(&gate, &sensitivity),
          "the accepted gate write must be dispatched exactly once");
  require(!state.take_static_radar_gate_request(&gate, &sensitivity),
          "a dispatched gate write must not be dispatched twice");
  state.mark_static_radar_gate_readback_pending(1500);
  require(!state.verify_static_radar_gate_readback(1500, NAN, NAN),
          "missing readback values must not be accepted");
  require(state.verify_static_radar_gate_readback(1501, 28.0f, 28.0f) &&
              state.static_radar_gate_operation_state == StaticRadarGateOperationState::APPLIED,
          "matching move and still thresholds should complete the operation");

  require(state.request_static_radar_gate(4, 60, &operation_id) == StaticRadarGateRequestResult::ACCEPTED &&
              operation_id == 2,
          "a verified operation should release the next gate write");
  require(state.take_static_radar_gate_request(&gate, &sensitivity), "second gate write should dispatch");
  state.mark_static_radar_gate_readback_pending(2500);
  require(state.verify_static_radar_gate_readback(2501, 40.0f, 41.0f) &&
              state.static_radar_gate_operation_state == StaticRadarGateOperationState::FAILED &&
              std::strcmp(state.static_radar_gate_operation_result, "readback_mismatch") == 0,
          "a partial LD2410C write must fail verification");

  require(state.request_static_radar_gate(5, 50, &operation_id) == StaticRadarGateRequestResult::ACCEPTED,
          "a failed operation should not permanently block tuning");
  require(state.take_static_radar_gate_request(&gate, &sensitivity), "timeout case should dispatch");
  state.mark_static_radar_gate_readback_pending(3500);
  require(!state.verify_static_radar_gate_readback(5499, NAN, NAN),
          "readback timeout must honor its full deadline");
  require(state.verify_static_radar_gate_readback(5500, NAN, NAN) &&
              state.static_radar_gate_operation_state == StaticRadarGateOperationState::FAILED &&
              std::strcmp(state.static_radar_gate_operation_result, "readback_timeout") == 0,
          "missing LD2410C readback must fail explicitly");
}

void strict_request_numbers_reject_partial_and_overflow_values() {
  using esphome::radar_api_server::parse_uint32_strict;
  uint32_t value = 0;
  require(parse_uint32_strict("4294967295", &value) && value == UINT32_MAX,
          "the maximum uint32 request value should parse");
  require(parse_uint32_strict("0x1234", &value, true, false) &&
              value == 0x1234,
          "hex upload sessions should remain compatible");
  require(!parse_uint32_strict("4294967296", &value) &&
              !parse_uint32_strict("12junk", &value) &&
              !parse_uint32_strict("-1", &value) &&
              !parse_uint32_strict(" 12", &value) &&
              !parse_uint32_strict("0", &value, false, false),
          "overflow, partial, signed, padded, and forbidden zero values must fail");
}

}  // namespace

int main(int argc, char **argv) {
  const bool role_suite = argc == 2 && std::strcmp(argv[1], "roles") == 0;
  const bool coordinator_suite =
      argc == 2 && std::strcmp(argv[1], "coordinators") == 0;
  const bool resource_suite =
      argc == 2 && std::strcmp(argv[1], "resources") == 0;
  const bool firmware_suite =
      argc == 2 && std::strcmp(argv[1], "firmware") == 0;
  if (argc > 1 && !role_suite && !coordinator_suite && !resource_suite &&
      !firmware_suite) {
    std::cerr << "tracker_tests: unknown suite\n";
    return 2;
  }
  if (firmware_suite) {
    run_firmware_update_security_tests();
    std::cout << "tracker_tests: firmware ok\n";
    return 0;
  }
  if (resource_suite) {
    run_shared_automation_area_tests();
    std::cout << "tracker_tests: resources ok\n";
    return 0;
  }
  if (coordinator_suite) {
    run_group_coordinator_tests();
    run_site_coordinator_tests();
    std::cout << "tracker_tests: coordinators ok\n";
    return 0;
  }
  if (role_suite) {
    run_credential_provisioning_tests();
    run_group_coordinator_tests();
    run_group_membership_tests();
    run_group_roster_tests();
    run_site_coordinator_tests();
    run_site_configuration_tests();
    run_site_authority_tests();
    run_site_directory_tests();
    run_site_group_summary_tests();
    run_site_heartbeat_tests();
    run_site_replication_tests();
    run_pairing_bootstrap_activation_tests();
    run_role_failure_matrix_tests();
    remote_fusion_accepts_a_verified_leader_reboot_term_reset();
    std::cout << "tracker_tests: roles ok\n";
    return 0;
  }
  platform_operation_lease_serializes_mutations();
  run_participant_registry_tests();
  run_participant_readiness_tests();
  run_participant_credentials_tests();
  run_participant_projection_tests();
  run_credential_provisioning_tests();
  run_participant_pairing_tests();
  run_pairing_bootstrap_activation_tests();
  run_participant_discovery_tests();
  run_platform_identity_tests();
  run_observation_transport_tests();
  run_group_coordinator_tests();
  run_group_membership_tests();
  run_group_roster_tests();
  run_site_coordinator_tests();
  run_site_configuration_tests();
  run_site_authority_tests();
  run_site_directory_tests();
  run_site_group_summary_tests();
  run_site_space_summary_tests();
  run_site_heartbeat_tests();
  run_site_replication_tests();
  run_ld2450_startup_guard_tests();
  run_ld245x_startup_sequence_tests();
  run_ld2410_stream_health_tests();
  run_spatial_shadow_fusion_tests();
  run_spatial_shadow_runtime_tests();
  run_spatial_production_fusion_tests();
  run_role_failure_matrix_tests();
  run_shared_automation_area_tests();
  run_firmware_update_security_tests();
  static_radar_factory_reset_request_is_single_shot_and_cancels_tuning();
  static_radar_gate_write_is_serialized_and_read_back();
  strict_request_numbers_reject_partial_and_overflow_values();
  no_detection_stays_idle();
  confirmation_requires_configured_hit_count();
  pir_hint_confirms_on_second_hit();
  reduced_zone_candidate_does_not_create_a_track();
  pir_cannot_create_a_reduced_zone_track();
  target_born_in_reduced_zone_starts_tracking_only_after_exit();
  confirmed_track_crosses_reduced_zone_without_changing_identity();
  persistent_reduced_zone_observation_expires_an_existing_track();
  coasting_track_reacquires_inside_reduced_zone_without_pir();
  duplicate_reduced_observations_do_not_split_an_existing_track();
  distant_reduced_zone_jump_does_not_move_or_replace_an_existing_track();
  reduced_zone_teleport_does_not_reanchor_a_transiting_track();
  pir_without_target_is_fusion_evidence_not_tracker_evidence();
  fusion_preserves_filter_and_pir_precedence();
  fusion_uses_tracker_evidence_and_drop_reason();
  static_radar_cannot_acquire_presence_by_itself();
  static_radar_detection_distance_rejects_single_sample_spikes();
  static_radar_detection_distance_accepts_persistent_changes_and_resets();
  static_radar_detection_distance_ignores_duplicate_ticks_and_unavailable_resets();
  static_radar_detection_distance_keeps_stable_value_during_invalid_samples();
  replay_log_serializes_canonical_static_distance();
  replay_log_serializes_reduced_zone_tracker_input();
  replay_log_serializes_per_target_filter_admission();
  static_assist_maintains_presence_after_unconfirmed_loss();
  static_assist_preserves_one_spatial_target_with_distance_confidence();
  static_assist_does_not_guess_between_multiple_spatial_targets();
  static_assist_does_not_hold_a_reduced_zone_coordinate();
  static_assist_combines_pir_and_tracker_reacquisition();
  static_assist_respects_exit_veto_and_ignores_stale_drop_events();
  static_assist_allows_one_reentry_window_after_stable_presence_ends();
  static_assist_reentry_window_expires_after_thirty_seconds();
  static_assist_reentry_window_respects_exit_and_filter_guards();
  static_assist_does_not_bypass_filter_or_missing_hardware();
  state_json_exposes_static_radar_fusion_evidence();
  fixed_state_json_builder_matches_string_contract();
  state_json_exposes_bounded_production_fusion_metadata();
  remote_fusion_accepts_a_verified_leader_reboot_term_reset();
  state_json_exposes_complete_automation_snapshot_without_false_clear();
  tentative_track_expires_after_miss_budget();
  confirmed_track_coasts_and_reacquires();
  filter_block_ages_track_without_consuming_detection();
  target_admission_blocks_only_the_filtered_target();
  filtered_false_positive_does_not_prevent_real_target_admission();
  non_exit_track_uses_long_coasting_budget();
  target_slot_reordering_preserves_stable_tracks();
  unmatched_detection_uses_free_track_slot();
  unexpected_jump_requires_three_consistent_measurements();
  ambiguous_distant_candidates_coast_without_splitting();
  pir_does_not_fast_confirm_an_additional_target();
  cold_start_two_slot_reflection_keeps_one_guarded_candidate();
  captured_reduced_reflection_does_not_become_a_second_person();
  secondary_coasting_expires_while_its_parent_remains_observed();
  kalman_filter_smooths_measurement_and_holds_coasting_position();
  radial_motion_sets_approaching_and_away_directions();
  room_signed_distance_is_positive_inside_and_negative_outside();
  device_config_cache_tracks_the_local_floorplan_pose_version();
  direct_upgrade_hydrates_floorplan_scale_without_moving_the_radar();
  software_zone_evidence_carries_room_signed_distance();
  zone_policy_and_evidence_share_cached_geometry();
  timezone_catalog_accepts_only_supported_ids();
  room_exit_sequence_expires_before_inside_room_hold();
  outside_without_crossing_does_not_create_room_exit();
  exit_evidence_still_expires_fastest();
  room_state_is_exposed_on_track_output();
  local_track_id_survives_coasting_and_reacquisition();
  tracker_time_metadata_survives_millis_wrap();
  local_track_ids_survive_sensor_slot_reordering();
  expired_track_gets_a_new_local_track_id();
  tentative_track_is_not_published_as_a_local_observation();
  local_observation_copies_tracks_and_optional_evidence();
  local_observation_remains_bounded_with_three_targets();
  local_observation_handles_missing_static_radar_and_filter_block();
  local_observation_is_shadow_only_and_sequence_advances();
  std::cout << "tracker_tests: ok\n";
  return 0;
}
