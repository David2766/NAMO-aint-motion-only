#pragma once

#include "fusion_snapshot.h"
#include "fusion_automation.h"
#include "group_coordinator.h"
#include "group_roster.h"
#include "observation_transport.h"
#include "participant_readiness.h"
#include "participant_registry_store.h"
#include "platform_operation.h"
#include "site_configuration.h"
#include "site_authority.h"
#include "site_coordinator.h"
#include "site_directory.h"
#include "site_group_summary.h"
#include "site_space_summary.h"
#include "shared_automation_area.h"
#include "spatial_production_fusion.h"
#include "spatial_shadow_runtime.h"

#include <array>

namespace esphome {
namespace radar_api_server {

struct InternalRamWatermarkDiagnostics {
  const char *source{"unobserved"};
  const char *phase{"unobserved"};
  const char *threshold_source{"unobserved"};
  const char *threshold_phase{"unobserved"};
  uint32_t minimum_free_bytes{0};
  uint32_t largest_free_block_bytes{0};
  uint32_t observed_at_ms{0};
  uint32_t event_count{0};
  uint32_t threshold_free_bytes{0};
  uint32_t threshold_observed_at_ms{0};
  uint8_t logged_threshold_level{0};
  bool initialized{false};
  bool threshold_recorded{false};
};

static_assert(sizeof(InternalRamWatermarkDiagnostics) <= 72,
              "internal RAM diagnostics exceeded its fixed budget");

struct PlatformCoordinatorPublishedSnapshot {
  uint32_t site_generation{0};
  SiteCoordinatorState site{};
  uint32_t group_generation{0};
  uint8_t group_count{0};
  std::array<GroupCoordinatorState, GroupCoordinator::MAX_GROUPS> groups{};
};

struct PlatformCoordinatorSnapshotLease {
  const PlatformCoordinatorPublishedSnapshot *snapshot{nullptr};
  uint8_t buffer_index{0};

  bool valid() const { return this->snapshot != nullptr; }
};

struct PlatformCoordinatorSnapshotStore {
  std::array<PlatformCoordinatorPublishedSnapshot, 2> snapshots{};
  std::array<uint8_t, 2> readers{};
  uint8_t active_index{0};
  bool published{false};

  bool acquire(PlatformCoordinatorSnapshotLease *lease) {
    if (lease == nullptr || !this->published ||
        this->readers[this->active_index] == 0xFFU)
      return false;
    lease->buffer_index = this->active_index;
    ++this->readers[lease->buffer_index];
    lease->snapshot = &this->snapshots[lease->buffer_index];
    return true;
  }

  void release(const PlatformCoordinatorSnapshotLease &lease) {
    if (!lease.valid() || lease.buffer_index >= this->snapshots.size() ||
        lease.snapshot != &this->snapshots[lease.buffer_index] ||
        this->readers[lease.buffer_index] == 0)
      return;
    --this->readers[lease.buffer_index];
  }

  bool prepare_publish(uint8_t *buffer_index) const {
    if (buffer_index == nullptr)
      return false;
    const uint8_t candidate = this->published ? this->active_index ^ 1U : 0U;
    if (this->readers[candidate] != 0)
      return false;
    *buffer_index = candidate;
    return true;
  }

  void commit_publish(uint8_t buffer_index) {
    if (buffer_index >= this->snapshots.size())
      return;
    this->active_index = buffer_index;
    this->published = true;
  }
};

static_assert(sizeof(PlatformCoordinatorSnapshotStore) <= 768,
              "coordinator snapshot store exceeded its PSRAM budget");

struct PlatformRuntimeWorkspace {
  ParticipantRegistrySnapshot participant_registry_snapshot{};
  SiteConfigurationSnapshot site_configuration_snapshot{};
  GroupRosterSnapshot group_roster_snapshot{};
  GroupRosterSnapshot group_roster_update_scratch{};
  SiteDirectorySnapshot site_directory_snapshot{};
  SiteDirectorySnapshot site_directory_update_scratch{};
  ParticipantReadinessSnapshot participant_readiness_snapshot{};
  PlatformParticipantOperationWorkspace participant_operation_workspace{};
  SiteConfigurationSnapshot site_configuration_update_scratch{};
  SiteAuthoritySnapshot site_authority_snapshot{};
  SiteAuthorityReplicationTracker site_authority_replication{};
  SiteCoordinator site_coordinator{};
  GroupCoordinator group_coordinator{};
  PlatformCoordinatorSnapshotStore coordinator_snapshots{};
  SiteGroupSummaryCache site_group_summaries{};
  SiteSpaceSummaryCache site_space_summaries{};
  SiteFloorplanOwnerCache site_floorplan_owners{};
  SpatialProductionFusion spatial_production_fusion{};
  FusionAutomationState fusion_automation{};
  SharedAutomationAreaCache shared_automation_areas{};
  InternalRamWatermarkDiagnostics internal_ram_watermark{};
  SpatialShadowRuntimeInput spatial_shadow_input_scratch{};
  std::array<const ParticipantRecord *, SpatialShadowRuntimeInput::MAX_REMOTES>
      spatial_remote_participant_scratch{};
  std::array<const ParticipantReadinessRecord *,
             SpatialShadowRuntimeInput::MAX_REMOTES>
      spatial_remote_readiness_scratch{};
  std::array<ObservationTransportSample,
             SpatialShadowRuntimeInput::MAX_REMOTES>
      remote_observation_scratch{};
  FusionSnapshot fusion_snapshot_scratch{};
};

static constexpr std::size_t PLATFORM_RUNTIME_WORKSPACE_BUDGET = 44U * 1024U;

static_assert(sizeof(PlatformRuntimeWorkspace) <=
                  PLATFORM_RUNTIME_WORKSPACE_BUDGET,
              "platform runtime workspace exceeded the PSRAM budget");
static_assert(SpatialShadowFusion::MAX_SOURCES ==
                  GroupMembershipView::MAX_PARTICIPANTS,
              "Group membership and fusion source capacities must match");

}  // namespace radar_api_server
}  // namespace esphome
