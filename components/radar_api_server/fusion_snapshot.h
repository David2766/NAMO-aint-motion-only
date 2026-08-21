#pragma once

#include "group_coordinator.h"
#include "participant_readiness.h"
#include "participant_registry.h"
#include "spatial_shadow_fusion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct FusionSnapshotTarget {
  bool valid{false};
  uint32_t fused_track_id{0};
  SpatialShadowTrackState state{SpatialShadowTrackState::EMPTY};
  uint8_t source_count{0};
  uint8_t confidence{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
};

struct FusionSnapshot {
  static constexpr uint32_t MIN_TTL_MS = 500;
  static constexpr uint32_t MAX_TTL_MS = 5000;
  static constexpr uint32_t DEFAULT_TTL_MS = 3000;
  static constexpr std::size_t MAX_TARGETS = 3;
  static constexpr std::size_t MAX_BOOT_ID_LENGTH = 48;

  bool occupied{false};
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char boot_id[MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t sequence{0};
  uint32_t group_term{0};
  uint32_t source_sequence{0};
  uint32_t received_at_ms{0};
  uint32_t expires_at_ms{0};
  uint32_t ttl_ms{0};
  uint8_t target_count{0};
  bool presence{false};
  bool motion{false};
  std::array<FusionSnapshotTarget, MAX_TARGETS> targets{};
};

enum class FusionSnapshotAdmissionResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  SITE_MISMATCH,
  UNKNOWN_PARTICIPANT,
  PARTICIPANT_INACTIVE,
  READINESS_UNAVAILABLE,
  GROUP_MISMATCH,
  LEADER_MISMATCH,
  TERM_MISMATCH,
};

const char *fusion_snapshot_admission_result_to_string(
    FusionSnapshotAdmissionResult result);

FusionSnapshotAdmissionResult evaluate_fusion_snapshot_admission(
    const FusionSnapshot &snapshot, const char *authenticated_participant_id,
    const char *local_site_id, const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupCoordinatorState *group);
FusionSnapshotAdmissionResult evaluate_fusion_snapshot_admission(
    const FusionSnapshot &snapshot, const char *authenticated_participant_id,
    const char *local_site_id, const ParticipantRecord *participant,
    const ParticipantReadinessRecord *readiness,
    const GroupCoordinatorState *group);

class FusionSnapshotCache {
 public:
  bool store(const FusionSnapshot &snapshot);
  bool copy(uint32_t now_ms, FusionSnapshot *snapshot) const;
  bool expire(uint32_t now_ms);
  void clear();

 private:
  FusionSnapshot snapshot_{};
};

static_assert(std::is_trivially_copyable<FusionSnapshot>::value,
              "fusion snapshot must not own heap-backed state");
static_assert(sizeof(FusionSnapshot) <= 384,
              "fusion snapshot exceeded the embedded memory budget");
static_assert(std::is_trivially_copyable<FusionSnapshotCache>::value,
              "fusion snapshot cache must not own heap-backed state");

}  // namespace radar_api_server
}  // namespace esphome
