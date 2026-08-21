#pragma once

#include "site_directory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct SiteGroupSummaryReport {
  const char *participant_id{nullptr};
  const char *group_id{nullptr};
  uint32_t assignment_revision{0};
  uint32_t term{0};
  uint32_t lease_ms{0};
  uint8_t participants_expected{0};
  uint8_t participants_online{0};
};

struct SiteGroupSummaryState {
  bool occupied{false};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char leader_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t assignment_revision{0};
  uint32_t term{0};
  uint32_t lease_expires_at_ms{0};
  uint8_t participants_expected{0};
  uint8_t participants_online{0};
};

enum class SiteGroupSummaryResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  DIRECTORY_NOT_READY,
  UNKNOWN_GROUP,
  LEADER_MISMATCH,
  REVISION_MISMATCH,
  MEMBER_COUNT_MISMATCH,
  FULL,
};

class SiteGroupSummaryCache {
 public:
  static constexpr std::size_t MAX_GROUPS = SiteDirectoryLimits::MAX_GROUPS;

  SiteGroupSummaryResult accept(
      const SiteDirectorySnapshot &directory,
      const SiteGroupSummaryReport &report, uint32_t now_ms);
  SiteGroupSummaryResult accept_discovered(
      const SiteConfigurationSnapshot &configuration,
      const SiteDirectorySnapshot *directory,
      const char *expected_leader_node_id, const char *floorplan_id,
      const SiteGroupSummaryReport &report, uint32_t now_ms);
  void reconcile(const SiteDirectorySnapshot *directory, bool local_authority,
                 uint32_t now_ms);
  void clear();

  const SiteGroupSummaryState *find(const char *group_id) const;
  const SiteGroupSummaryState *summary_at(std::size_t slot) const;
  std::size_t size() const;
  uint32_t generation() const { return this->generation_; }
  uint32_t rejected() const { return this->rejected_; }
  uint32_t expired() const { return this->expired_; }

  static uint32_t lease_remaining_ms(const SiteGroupSummaryState &state,
                                     uint32_t now_ms);

 private:
  std::array<SiteGroupSummaryState, MAX_GROUPS> summaries_{};
  uint32_t generation_{0};
  uint32_t rejected_{0};
  uint32_t expired_{0};

  SiteGroupSummaryState *find_mutable_(const char *group_id);
  SiteGroupSummaryState *ensure_(const char *group_id);
  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
  static bool valid_text_(const char *value, std::size_t maximum);
  static void copy_(char *target, std::size_t capacity, const char *source);
};

const char *site_group_summary_result_to_string(
    SiteGroupSummaryResult result);

static_assert(std::is_trivially_copyable<SiteGroupSummaryCache>::value,
              "Site Group summaries must remain fixed-size runtime state");
static_assert(sizeof(SiteGroupSummaryCache) <= 2048,
              "Site Group summary cache exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
