#pragma once

#include "fusion_snapshot.h"
#include "spatial_production_fusion.h"
#include "spatial_transform.h"

#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class FusionAutomationHealth : uint8_t {
  DISABLED = 0,
  UNAVAILABLE,
  DEGRADED,
  RECOVERING,
  HEALTHY,
};

enum class FusionAutomationSource : uint8_t {
  NONE = 0,
  LOCAL_GROUP_LEADER,
  GROUP_LEADER_SNAPSHOT,
};

const char *fusion_automation_health_to_string(FusionAutomationHealth health);
const char *fusion_automation_source_to_string(FusionAutomationSource source);

struct FusionAutomationSnapshot {
  bool build_enabled{false};
  bool authoritative{false};
  FusionAutomationHealth health{FusionAutomationHealth::DISABLED};
  FusionAutomationSource source{FusionAutomationSource::NONE};
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char boot_id[FusionSnapshot::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t group_term{0};
  uint32_t sequence{0};
  uint32_t source_sequence{0};
  uint32_t updated_at_ms{0};
  uint32_t expires_at_ms{0};
  bool continuity_held{false};
  char continuity_source_leader_node_id[
      PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t continuity_source_group_term{0};
  uint32_t continuity_started_at_ms{0};
  uint32_t continuity_expires_at_ms{0};
  bool presence{false};
  bool motion{false};
  SpatialProductionFusionOutput fusion{};
};

class FusionAutomationState {
 public:
  static constexpr uint32_t CONTINUITY_HOLD_MS = 30000;

  void disable(const char *site_id = "", const char *space_id = "",
               const char *group_id = "", const char *leader_node_id = "",
               uint32_t group_term = 0);
  void mark_unavailable(const char *site_id, const char *space_id,
                         const char *group_id, const char *leader_node_id,
                         uint32_t group_term, bool degraded);
  bool mark_recovering(const char *site_id, const char *space_id,
                       const char *group_id, const char *leader_node_id,
                       uint32_t group_term, uint32_t now_ms);
  bool preserve_continuity_or_mark_unavailable(
      const char *site_id, const char *space_id, const char *group_id,
      const char *leader_node_id, uint32_t group_term, uint32_t now_ms,
      bool degraded);
  bool update_local(const char *site_id, const char *space_id,
                    const char *group_id, const char *leader_node_id,
                    const char *boot_id, uint32_t group_term,
                    const SpatialProductionFusionOutput &fusion,
                    uint32_t now_ms);
  bool update_remote(const FusionSnapshot &snapshot, const char *space_id,
                     const SpatialTransform2D &local_transform,
                     bool authority_admitted = false);

  const FusionAutomationSnapshot &snapshot() const { return this->snapshot_; }
  const SpatialProductionFusionOutput &fusion_output() const {
    return this->snapshot_.fusion;
  }
  static uint32_t continuity_age_ms(
      const FusionAutomationSnapshot &snapshot, uint32_t now_ms);
  static uint32_t continuity_remaining_ms(
      const FusionAutomationSnapshot &snapshot, uint32_t now_ms);

 private:
  FusionAutomationSnapshot snapshot_{};
};

static_assert(std::is_trivially_copyable<FusionAutomationState>::value,
              "fusion automation state must not own heap-backed state");
static_assert(sizeof(FusionAutomationState) <= 768,
              "fusion automation state exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
