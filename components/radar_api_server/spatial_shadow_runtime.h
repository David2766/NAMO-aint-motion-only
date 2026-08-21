#pragma once

#include "local_observation.h"
#include "observation_transport.h"
#include "spatial_shadow_fusion.h"
#include "spatial_transform.h"

#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class SpatialShadowRuntimeState : uint8_t {
  DISABLED = 0,
  GROUP_UNAVAILABLE,
  NOT_GROUP_LEADER,
  SOURCE_COUNT_UNSUPPORTED,
  LOCAL_TRANSFORM_UNAVAILABLE,
  LOCAL_POSE_UNAVAILABLE,
  LOCAL_OBSERVATION_UNAVAILABLE,
  READINESS_MISMATCH,
  REMOTE_OBSERVATION_UNAVAILABLE,
  ACTIVE,
};

const char *spatial_shadow_runtime_state_to_string(
    SpatialShadowRuntimeState state);

uint64_t spatial_shadow_participant_token(const char *participant_id);

struct SpatialShadowRemoteInput {
  const char *participant_id{nullptr};
  uint64_t participant_token{0};
  uint32_t expected_pose_version{0};
  uint32_t expected_calibration_version{0};
  SpatialTransform2D transform{};
  const ObservationTransportSample *observation{nullptr};
};

struct SpatialShadowRuntimeInput {
  static constexpr std::size_t MAX_REMOTES =
      SpatialShadowFusion::MAX_SOURCES - 1;

  bool enabled{false};
  bool group_configured{false};
  bool local_group_leader{false};
  bool source_count_supported{false};
  bool local_transform_authoritative{false};
  const char *group_id{nullptr};
  const char *local_participant_id{nullptr};
  uint32_t local_pose_version{0};
  uint32_t expected_local_pose_version{0};
  SpatialTransform2D local_transform{};
  const LocalObservation *local_observation{nullptr};
  uint8_t remote_count{0};
  std::array<SpatialShadowRemoteInput, MAX_REMOTES> remotes{};
};

struct SpatialShadowRuntimeDiagnostics {
  static constexpr bool SHADOW_ONLY = true;
  static constexpr bool PRODUCTION_AUTHORITY = false;

  SpatialShadowRuntimeState state{SpatialShadowRuntimeState::DISABLED};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char remote_participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t updates{0};
  uint32_t skipped{0};
  uint32_t max_cycle_us{0};
};

class SpatialShadowRuntime {
 public:
  static constexpr uint32_t TRANSIENT_HOLD_MS = 3000;

  void update(uint32_t now_ms, const SpatialShadowRuntimeInput &input);
  void clear();
  void record_cycle_us(uint32_t cycle_us);

  const SpatialShadowRuntimeDiagnostics &diagnostics() const {
    return this->diagnostics_;
  }
  const SpatialShadowFusionOutput &output() const {
    return this->fusion_.output();
  }

 private:
  SpatialShadowFusion fusion_{};
  std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES>
      source_scratch_{};
  SpatialShadowRuntimeDiagnostics diagnostics_{};
  uint8_t last_source_count_{0};
  std::array<uint64_t, SpatialShadowFusion::MAX_SOURCES>
      last_source_tokens_{};
  std::array<uint32_t, SpatialShadowFusion::MAX_SOURCES>
      last_source_sequences_{};
  std::array<uint32_t, SpatialShadowFusion::MAX_SOURCES>
      source_sequence_scratch_{};
  char active_group_id_[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH +
                        1]{};
  uint32_t active_local_pose_version_{0};
  SpatialTransform2D active_local_transform_{};
  uint32_t transient_started_ms_{0};

  void hard_stop_(SpatialShadowRuntimeState state,
                  const SpatialShadowRuntimeInput &input);
  void pause_(uint32_t now_ms, SpatialShadowRuntimeState state,
              const SpatialShadowRuntimeInput &input);
  void begin_active_scope_(const SpatialShadowRuntimeInput &input);
  static SpatialShadowSource local_source_(
      uint32_t now_ms, const SpatialShadowRuntimeInput &input);
  static SpatialShadowSource remote_source_(
      const SpatialShadowRemoteInput &input);
  static void copy_(char *target, std::size_t capacity, const char *source);
};

static_assert(SpatialShadowRuntimeDiagnostics::SHADOW_ONLY,
              "runtime diagnostics must remain shadow-only");
static_assert(!SpatialShadowRuntimeDiagnostics::PRODUCTION_AUTHORITY,
              "runtime diagnostics must not gain production authority");
static_assert(std::is_trivially_copyable<SpatialShadowRuntime>::value,
              "shadow runtime must not own heap-backed state");
static_assert(sizeof(SpatialShadowRuntime) <= 8192,
              "shadow runtime exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
