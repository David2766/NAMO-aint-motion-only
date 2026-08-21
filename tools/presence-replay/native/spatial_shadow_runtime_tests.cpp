#include "spatial_shadow_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "spatial_shadow_runtime_tests: %s\n", message);
  std::exit(1);
}

SpatialTransform2D transform(int32_t x_mm, int32_t y_mm,
                             int16_t rotation_cdeg) {
  SpatialTransform2D value;
  value.origin_x_mm = x_mm;
  value.origin_y_mm = y_mm;
  value.rotation_cdeg = rotation_cdeg;
  value.residual_mm = 0;
  value.sample_count = 1;
  value.valid = true;
  return value;
}

LocalObservation local_observation(uint32_t sequence) {
  LocalObservation value;
  value.sequence = sequence;
  value.observed_at_ms = 1000;
  value.ttl_ms = 1500;
  value.track_count = 1;
  value.tracks[0].valid = true;
  value.tracks[0].local_track_id = 10;
  value.tracks[0].x_mm = 0.0f;
  value.tracks[0].y_mm = 1000.0f;
  value.tracks[0].position_variance_x_mm2 = 40000.0f;
  value.tracks[0].position_variance_y_mm2 = 40000.0f;
  value.tracks[0].track_score = 90;
  return value;
}

ObservationTransportSample remote_observation(uint32_t sequence) {
  ObservationTransportSample value;
  value.occupied = true;
  std::snprintf(value.participant_id, sizeof(value.participant_id), "node-b");
  value.participant_token = 22;
  value.sequence = sequence;
  value.sample_age_ms = 0;
  value.ttl_ms = 1500;
  value.pose_version = 4;
  value.calibration_version = 5;
  value.track_count = 1;
  value.tracks[0].valid = true;
  value.tracks[0].local_track_id = 20;
  value.tracks[0].x_mm = 0.0f;
  value.tracks[0].y_mm = 1000.0f;
  value.tracks[0].uncertainty_available = true;
  value.tracks[0].variance_x_mm2 = 40000.0f;
  value.tracks[0].variance_y_mm2 = 40000.0f;
  value.tracks[0].quality_score = 90;
  return value;
}

SpatialShadowRuntimeInput valid_input(const LocalObservation *local,
                                      const ObservationTransportSample *remote) {
  SpatialShadowRuntimeInput input;
  input.enabled = true;
  input.group_configured = true;
  input.local_group_leader = true;
  input.source_count_supported = true;
  input.local_transform_authoritative = true;
  input.group_id = "group-main";
  input.local_participant_id = "node-a";
  input.local_pose_version = 3;
  input.expected_local_pose_version = 3;
  input.local_transform = transform(1000, 1000, 0);
  input.local_observation = local;
  input.remote_count = 1;
  input.remotes[0].participant_id = "node-b";
  input.remotes[0].participant_token = 22;
  input.remotes[0].expected_pose_version = 4;
  input.remotes[0].expected_calibration_version = 5;
  input.remotes[0].transform = transform(1050, 1000, 0);
  input.remotes[0].observation = remote;
  return input;
}

void runtime_fuses_only_new_admitted_sequences() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  const auto input = valid_input(&local, &remote);
  runtime.update(1000, input);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE,
          "valid leader input should activate shadow runtime");
  require(runtime.output().track_count == 1 &&
              runtime.output().tracks[0].source_count == 2,
          "nearby world-coordinate observations should form one candidate");
  const uint32_t output_sequence = runtime.output().sequence;
  runtime.update(1050, input);
  require(runtime.output().sequence == output_sequence &&
              runtime.diagnostics().updates == 1 &&
              runtime.diagnostics().skipped == 1,
          "the same transport sequences must not confirm a track repeatedly");

  local.sequence = 2;
  remote.sequence = 2;
  runtime.update(1100, valid_input(&local, &remote));
  require(runtime.output().tracks[0].state ==
              SpatialShadowTrackState::CONFIRMED &&
              runtime.diagnostics().updates == 2,
          "a second real observation should confirm the fused track");
}

void runtime_fails_closed_outside_group_leader() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  auto input = valid_input(&local, &remote);
  runtime.update(1000, input);
  input.local_group_leader = false;
  runtime.update(1100, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::NOT_GROUP_LEADER &&
              runtime.output().track_count == 0,
          "a follower must clear its local shadow fusion output");
}

void runtime_keeps_local_authority_when_the_last_remote_disappears() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  runtime.update(1000, valid_input(&local, &remote));
  const uint32_t first_sequence = runtime.output().sequence;
  const uint32_t first_track_id = runtime.output().tracks[0].fused_track_id;

  local.sequence = 2;
  auto local_only = valid_input(&local, &remote);
  local_only.remote_count = 0;
  runtime.update(1100, local_only);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().track_count == 1 &&
              runtime.output().tracks[0].source_count == 1 &&
              runtime.output().tracks[0].fused_track_id == first_track_id &&
              runtime.output().sequence > first_sequence,
          "the surviving Group Leader must retain local-only authority");
  const uint32_t local_only_sequence = runtime.output().sequence;

  local.sequence = 3;
  remote.sequence = 2;
  runtime.update(1200, valid_input(&local, &remote));
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().sequence > local_only_sequence &&
              runtime.output().tracks[0].fused_track_id == first_track_id,
          "remote rejoin must preserve the local-only track identity");
}

void runtime_keeps_fusing_when_the_leader_sensor_is_temporarily_stale() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  runtime.update(1000, valid_input(&local, &remote));
  local.sequence = 2;
  remote.sequence = 2;
  runtime.update(1100, valid_input(&local, &remote));
  const uint32_t fused_id = runtime.output().tracks[0].fused_track_id;

  local.observed_at_ms = 1000;
  remote.sequence = 3;
  auto input = valid_input(&local, &remote);
  runtime.update(2601, input);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().track_count == 1 &&
              runtime.output().tracks[0].fused_track_id == fused_id &&
              runtime.output().tracks[0].source_count == 1,
          "a stale Group Leader sensor must not stop fresh remote fusion");
}

void runtime_preserves_tracks_across_a_bounded_total_input_gap() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  runtime.update(1000, valid_input(&local, &remote));
  local.sequence = 2;
  remote.sequence = 2;
  runtime.update(1100, valid_input(&local, &remote));
  const uint32_t fused_id = runtime.output().tracks[0].fused_track_id;

  local.observed_at_ms = 1000;
  remote.sample_age_ms = remote.ttl_ms + 1;
  runtime.update(2601, valid_input(&local, &remote));
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::LOCAL_OBSERVATION_UNAVAILABLE &&
              runtime.output().track_count == 1,
          "a total data gap should fail closed without immediately erasing tracks");

  local.sequence = 3;
  local.observed_at_ms = 3000;
  remote.sequence = 3;
  remote.sample_age_ms = 0;
  runtime.update(3000, valid_input(&local, &remote));
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().tracks[0].fused_track_id == fused_id,
          "fresh input within the bounded gap must restore the same fused ID");
}

void runtime_clears_tracks_after_a_prolonged_total_input_gap() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  runtime.update(1000, valid_input(&local, &remote));
  local.sequence = 2;
  remote.sequence = 2;
  runtime.update(1100, valid_input(&local, &remote));

  local.observed_at_ms = 1000;
  remote.sample_age_ms = remote.ttl_ms + 1;
  const auto input = valid_input(&local, &remote);
  runtime.update(2601, input);
  runtime.update(2601 + SpatialShadowRuntime::TRANSIENT_HOLD_MS + 1, input);
  require(runtime.output().track_count == 0,
          "a prolonged total data gap must eventually release stale tracks");
}

void runtime_skips_one_unready_remote_without_stopping_other_sources() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  auto input = valid_input(&local, &remote);
  input.remotes[0].expected_pose_version = remote.pose_version + 1;
  runtime.update(1000, input);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().track_count == 1 &&
              runtime.output().tracks[0].source_count == 1,
          "one unready remote must be excluded without stopping local fusion");
}

void runtime_starts_with_one_local_source() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto input = valid_input(&local, nullptr);
  input.remote_count = 0;
  runtime.update(1000, input);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().track_count == 1 &&
              runtime.output().tracks[0].source_count == 1 &&
              runtime.diagnostics().remote_participant_id[0] == '\0',
          "a configured Group with one online Node must start local-only");
}

void runtime_rejects_pose_and_source_count_mismatch() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  auto input = valid_input(&local, &remote);
  input.expected_local_pose_version = 9;
  runtime.update(1000, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::READINESS_MISMATCH,
          "local pose revision mismatch must fail closed");
  input = valid_input(&local, &remote);
  input.source_count_supported = false;
  runtime.update(1100, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED,
          "an explicitly unsupported source count must fail closed");
  input = valid_input(&local, &remote);
  input.local_transform_authoritative = false;
  runtime.update(1200, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::LOCAL_TRANSFORM_UNAVAILABLE,
          "a Group Leader without its own approved transform must fail closed");
}

void runtime_accepts_seven_bounded_sources() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  std::array<ObservationTransportSample,
             SpatialShadowRuntimeInput::MAX_REMOTES>
      remotes{};
  auto input = valid_input(&local, &remotes[0]);
  input.remote_count = static_cast<uint8_t>(remotes.size());
  for (std::size_t index = 0; index < remotes.size(); ++index) {
    remotes[index] = remote_observation(1);
    remotes[index].tracks[0].local_track_id =
        static_cast<uint32_t>(20 + index);
    auto &remote = input.remotes[index];
    remote.participant_id = "remote";
    remote.participant_token = static_cast<uint64_t>(22 + index);
    remote.expected_pose_version = 4;
    remote.expected_calibration_version = 5;
    remote.transform =
        transform(1000 + static_cast<int32_t>(index * 20), 1000, 0);
    remote.observation = &remotes[index];
  }

  runtime.update(1000, input);
  require(runtime.diagnostics().state == SpatialShadowRuntimeState::ACTIVE &&
              runtime.output().track_count == 1 &&
              runtime.output().tracks[0].source_count == 7,
          "one local and six remote sources must fit the bounded runtime");

  local.sequence = 2;
  for (auto &remote : remotes)
    remote.sequence = 2;
  runtime.update(1100, input);
  require(runtime.output().tracks[0].state ==
              SpatialShadowTrackState::CONFIRMED,
          "seven-source evidence should retain the normal confirmation lifecycle");
}

void runtime_rejects_an_eighth_source_before_array_access() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  auto remote = remote_observation(1);
  auto input = valid_input(&local, &remote);
  input.remote_count =
      static_cast<uint8_t>(SpatialShadowRuntimeInput::MAX_REMOTES + 1);

  runtime.update(1000, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::SOURCE_COUNT_UNSUPPORTED &&
              runtime.output().track_count == 0,
          "an eighth source must fail closed before remote array access");
}

void runtime_rejects_duplicate_remote_tokens() {
  SpatialShadowRuntime runtime;
  auto local = local_observation(1);
  std::array<ObservationTransportSample, 2> remotes{
      remote_observation(1), remote_observation(1)};
  auto input = valid_input(&local, &remotes[0]);
  input.remote_count = 2;
  input.remotes[1] = input.remotes[0];
  input.remotes[1].participant_id = "node-c";
  input.remotes[1].observation = &remotes[1];

  runtime.update(1000, input);
  require(runtime.diagnostics().state ==
              SpatialShadowRuntimeState::READINESS_MISMATCH,
          "duplicate authenticated participant tokens must fail closed");
}

void runtime_identity_token_is_stable_and_nonzero() {
  const auto first = spatial_shadow_participant_token("namo-node-a");
  require(first != 0 &&
              first == spatial_shadow_participant_token("namo-node-a") &&
              first != spatial_shadow_participant_token("namo-node-b"),
          "local shadow source token must be stable and participant-specific");
}

}  // namespace

void run_spatial_shadow_runtime_tests() {
  runtime_fuses_only_new_admitted_sequences();
  runtime_fails_closed_outside_group_leader();
  runtime_keeps_local_authority_when_the_last_remote_disappears();
  runtime_keeps_fusing_when_the_leader_sensor_is_temporarily_stale();
  runtime_preserves_tracks_across_a_bounded_total_input_gap();
  runtime_clears_tracks_after_a_prolonged_total_input_gap();
  runtime_skips_one_unready_remote_without_stopping_other_sources();
  runtime_starts_with_one_local_source();
  runtime_rejects_pose_and_source_count_mismatch();
  runtime_accepts_seven_bounded_sources();
  runtime_rejects_an_eighth_source_before_array_access();
  runtime_rejects_duplicate_remote_tokens();
  runtime_identity_token_is_stable_and_nonzero();
}
