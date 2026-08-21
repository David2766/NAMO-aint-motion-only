#pragma once

#include "group_coordinator.h"
#include "site_configuration.h"
#include "site_coordinator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct SpatialProductionFusionOutput;

struct SiteSpaceSummary {
  static constexpr uint32_t DEFAULT_TTL_MS = 6000;
  static constexpr uint32_t MIN_TTL_MS = 2000;
  static constexpr uint32_t MAX_TTL_MS = 6000;

  bool occupied{false};
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t sequence{0};
  uint32_t group_term{0};
  uint32_t source_sequence{0};
  uint32_t ttl_ms{DEFAULT_TTL_MS};
  uint8_t target_count{0};
  bool presence{false};
  bool motion{false};
};

bool build_site_space_summary(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, const char *boot_id, uint32_t sequence,
    uint32_t group_term, const SpatialProductionFusionOutput &fusion,
    SiteSpaceSummary *summary);

enum class SiteSpaceSummaryAdmissionResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  NOT_SITE_LEADER,
  SITE_MISMATCH,
  SENDER_MISMATCH,
  PARTICIPANT_INACTIVE,
  GROUP_UNAVAILABLE,
  GROUP_LEADER_MISMATCH,
  GROUP_TERM_MISMATCH,
  SPACE_MISMATCH,
  DUPLICATE_OR_REORDERED,
  FULL,
};

const char *site_space_summary_admission_result_to_string(
    SiteSpaceSummaryAdmissionResult result);

SiteSpaceSummaryAdmissionResult evaluate_site_space_summary_admission(
    const SiteSpaceSummary &summary, const char *authenticated_sender,
    const char *local_participant_id, const char *site_id,
    const SiteCoordinatorState &site,
    const SiteConfigurationSnapshot *configuration,
    const GroupCoordinatorState *group);

struct SiteSpaceSummaryState {
  bool occupied{false};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char leader_node_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char boot_id[GroupHeartbeat::MAX_BOOT_ID_LENGTH + 1]{};
  uint32_t sequence{0};
  uint32_t group_term{0};
  uint32_t source_sequence{0};
  uint32_t received_at_ms{0};
  uint32_t expires_at_ms{0};
  uint8_t target_count{0};
  bool presence{false};
  bool motion{false};
};

class SiteSpaceSummaryCache {
 public:
  static constexpr std::size_t MAX_SPACES = 8;

  SiteSpaceSummaryAdmissionResult store(const SiteSpaceSummary &summary,
                                        uint32_t now_ms);
  void expire(uint32_t now_ms);
  void clear();

  const SiteSpaceSummaryState *find(const char *group_id) const;
  const SiteSpaceSummaryState *summary_at(std::size_t slot) const;
  std::size_t size() const;
  uint32_t generation() const { return this->generation_; }
  uint32_t admitted() const { return this->admitted_; }
  uint32_t rejected() const { return this->rejected_; }
  uint32_t expired() const { return this->expired_; }
  static uint32_t age_ms(const SiteSpaceSummaryState &state,
                         uint32_t now_ms);

 private:
  std::array<SiteSpaceSummaryState, MAX_SPACES> summaries_{};
  uint32_t generation_{0};
  uint32_t admitted_{0};
  uint32_t rejected_{0};
  uint32_t expired_{0};

  SiteSpaceSummaryState *find_mutable_(const char *group_id);
  SiteSpaceSummaryState *ensure_(const char *group_id);
};

static_assert(std::is_trivially_copyable<SiteSpaceSummary>::value,
              "Site space summaries must remain fixed-size values");
static_assert(std::is_trivially_copyable<SiteSpaceSummaryCache>::value,
              "Site space summary cache must not own heap-backed state");
static_assert(sizeof(SiteSpaceSummaryCache) <= 3072,
              "Site space summary cache exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
