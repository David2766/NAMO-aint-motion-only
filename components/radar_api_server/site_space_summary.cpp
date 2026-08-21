#include "site_space_summary.h"

#include "spatial_production_fusion.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

bool valid_text(const char *value, std::size_t maximum) {
  if (value == nullptr)
    return false;
  const std::size_t length = std::strlen(value);
  if (length == 0 || length > maximum)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const char character = value[index];
    const bool alphanumeric =
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9');
    if (!alphanumeric && character != '-' && character != '_' &&
        character != '.' && character != ':')
      return false;
  }
  return true;
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

bool same_text(const char *left, const char *right) {
  return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

}  // namespace

bool build_site_space_summary(
    const char *site_id, const char *space_id, const char *group_id,
    const char *leader_node_id, const char *boot_id, uint32_t sequence,
    uint32_t group_term, const SpatialProductionFusionOutput &fusion,
    SiteSpaceSummary *summary) {
  if (summary == nullptr ||
      !valid_text(site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_text(space_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(group_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(leader_node_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_text(boot_id, GroupHeartbeat::MAX_BOOT_ID_LENGTH) ||
      sequence == 0 || group_term == 0 || !fusion.build_enabled ||
      !fusion.authoritative ||
      fusion.state != SpatialProductionFusionState::ACTIVE ||
      fusion.source_sequence == 0)
    return false;

  SiteSpaceSummary built{};
  built.occupied = true;
  copy_text(built.site_id, sizeof(built.site_id), site_id);
  copy_text(built.space_id, sizeof(built.space_id), space_id);
  copy_text(built.group_id, sizeof(built.group_id), group_id);
  copy_text(built.leader_node_id, sizeof(built.leader_node_id),
            leader_node_id);
  copy_text(built.boot_id, sizeof(built.boot_id), boot_id);
  built.sequence = sequence;
  built.group_term = group_term;
  built.source_sequence = fusion.source_sequence;

  for (const auto &target : fusion.targets) {
    if (!target.valid ||
        (target.state != SpatialShadowTrackState::CONFIRMED &&
         target.state != SpatialShadowTrackState::COASTING))
      continue;
    ++built.target_count;
    built.motion = built.motion ||
                   std::hypot(target.vx_mm_s, target.vy_mm_s) > 100.0f;
  }
  built.presence = built.target_count > 0;
  *summary = built;
  return true;
}

const char *site_space_summary_admission_result_to_string(
    SiteSpaceSummaryAdmissionResult result) {
  switch (result) {
    case SiteSpaceSummaryAdmissionResult::OK: return "ok";
    case SiteSpaceSummaryAdmissionResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case SiteSpaceSummaryAdmissionResult::NOT_SITE_LEADER:
      return "not_site_leader";
    case SiteSpaceSummaryAdmissionResult::SITE_MISMATCH:
      return "site_mismatch";
    case SiteSpaceSummaryAdmissionResult::SENDER_MISMATCH:
      return "sender_mismatch";
    case SiteSpaceSummaryAdmissionResult::PARTICIPANT_INACTIVE:
      return "participant_inactive";
    case SiteSpaceSummaryAdmissionResult::GROUP_UNAVAILABLE:
      return "group_unavailable";
    case SiteSpaceSummaryAdmissionResult::GROUP_LEADER_MISMATCH:
      return "group_leader_mismatch";
    case SiteSpaceSummaryAdmissionResult::GROUP_TERM_MISMATCH:
      return "group_term_mismatch";
    case SiteSpaceSummaryAdmissionResult::SPACE_MISMATCH:
      return "space_mismatch";
    case SiteSpaceSummaryAdmissionResult::DUPLICATE_OR_REORDERED:
      return "duplicate_or_reordered";
    case SiteSpaceSummaryAdmissionResult::FULL: return "full";
  }
  return "invalid_argument";
}

SiteSpaceSummaryAdmissionResult evaluate_site_space_summary_admission(
    const SiteSpaceSummary &summary, const char *authenticated_sender,
    const char *local_participant_id, const char *site_id,
    const SiteCoordinatorState &site,
    const SiteConfigurationSnapshot *configuration,
    const GroupCoordinatorState *group) {
  if (!summary.occupied ||
      !valid_text(summary.site_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_text(summary.space_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(summary.group_id,
                  ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      !valid_text(summary.leader_node_id, PlatformIdentity::MAX_ID_LENGTH) ||
      !valid_text(summary.boot_id, GroupHeartbeat::MAX_BOOT_ID_LENGTH) ||
      authenticated_sender == nullptr || local_participant_id == nullptr ||
      site_id == nullptr || summary.sequence == 0 || summary.group_term == 0 ||
      summary.source_sequence == 0 ||
      summary.ttl_ms < SiteSpaceSummary::MIN_TTL_MS ||
      summary.ttl_ms > SiteSpaceSummary::MAX_TTL_MS ||
      summary.target_count > SpatialProductionFusionOutput::MAX_TARGETS)
    return SiteSpaceSummaryAdmissionResult::INVALID_ARGUMENT;
  if (!site.local_leader ||
      !same_text(site.leader_node_id, local_participant_id))
    return SiteSpaceSummaryAdmissionResult::NOT_SITE_LEADER;
  if (!same_text(summary.site_id, site_id))
    return SiteSpaceSummaryAdmissionResult::SITE_MISMATCH;
  if (!same_text(summary.leader_node_id, authenticated_sender))
    return SiteSpaceSummaryAdmissionResult::SENDER_MISMATCH;
  if (configuration == nullptr)
    return SiteSpaceSummaryAdmissionResult::PARTICIPANT_INACTIVE;
  const auto *member = configuration->find(authenticated_sender);
  if (member == nullptr ||
      member->membership != ParticipantMembership::ACTIVE ||
      !member->leader_eligible || !member->spatial_profile.committed)
    return SiteSpaceSummaryAdmissionResult::PARTICIPANT_INACTIVE;
  if (group == nullptr || !group->configured || group->group_id[0] == '\0')
    return SiteSpaceSummaryAdmissionResult::GROUP_UNAVAILABLE;
  if (!same_text(group->group_id, summary.group_id) ||
      !same_text(group->leader_node_id, authenticated_sender))
    return SiteSpaceSummaryAdmissionResult::GROUP_LEADER_MISMATCH;
  if (group->term != summary.group_term)
    return SiteSpaceSummaryAdmissionResult::GROUP_TERM_MISMATCH;
  if (!same_text(member->spatial_profile.group_id, summary.group_id) ||
      !same_text(member->spatial_profile.space_id, summary.space_id))
    return SiteSpaceSummaryAdmissionResult::SPACE_MISMATCH;
  return SiteSpaceSummaryAdmissionResult::OK;
}

SiteSpaceSummaryAdmissionResult SiteSpaceSummaryCache::store(
    const SiteSpaceSummary &summary, uint32_t now_ms) {
  auto *target = this->ensure_(summary.group_id);
  if (target == nullptr) {
    ++this->rejected_;
    return SiteSpaceSummaryAdmissionResult::FULL;
  }
  if (target->occupied && same_text(target->boot_id, summary.boot_id) &&
      summary.sequence <= target->sequence) {
    ++this->rejected_;
    return SiteSpaceSummaryAdmissionResult::DUPLICATE_OR_REORDERED;
  }
  *target = {};
  target->occupied = true;
  copy_text(target->space_id, sizeof(target->space_id), summary.space_id);
  copy_text(target->group_id, sizeof(target->group_id), summary.group_id);
  copy_text(target->leader_node_id, sizeof(target->leader_node_id),
            summary.leader_node_id);
  copy_text(target->boot_id, sizeof(target->boot_id), summary.boot_id);
  target->sequence = summary.sequence;
  target->group_term = summary.group_term;
  target->source_sequence = summary.source_sequence;
  target->received_at_ms = now_ms;
  target->expires_at_ms = now_ms + summary.ttl_ms;
  target->target_count = summary.target_count;
  target->presence = summary.presence;
  target->motion = summary.motion;
  ++this->generation_;
  ++this->admitted_;
  return SiteSpaceSummaryAdmissionResult::OK;
}

void SiteSpaceSummaryCache::expire(uint32_t now_ms) {
  bool changed = false;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied || !time_reached(now_ms, summary.expires_at_ms))
      continue;
    summary = {};
    ++this->expired_;
    changed = true;
  }
  if (changed)
    ++this->generation_;
}

void SiteSpaceSummaryCache::clear() {
  bool changed = false;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied)
      continue;
    summary = {};
    changed = true;
  }
  if (changed)
    ++this->generation_;
}

const SiteSpaceSummaryState *SiteSpaceSummaryCache::find(
    const char *group_id) const {
  if (group_id == nullptr)
    return nullptr;
  for (const auto &summary : this->summaries_) {
    if (summary.occupied && same_text(summary.group_id, group_id))
      return &summary;
  }
  return nullptr;
}

const SiteSpaceSummaryState *SiteSpaceSummaryCache::summary_at(
    std::size_t slot) const {
  return slot < this->summaries_.size() && this->summaries_[slot].occupied
             ? &this->summaries_[slot]
             : nullptr;
}

std::size_t SiteSpaceSummaryCache::size() const {
  std::size_t count = 0;
  for (const auto &summary : this->summaries_)
    count += summary.occupied ? 1U : 0U;
  return count;
}

uint32_t SiteSpaceSummaryCache::age_ms(const SiteSpaceSummaryState &state,
                                       uint32_t now_ms) {
  return now_ms - state.received_at_ms;
}

SiteSpaceSummaryState *SiteSpaceSummaryCache::find_mutable_(
    const char *group_id) {
  return const_cast<SiteSpaceSummaryState *>(this->find(group_id));
}

SiteSpaceSummaryState *SiteSpaceSummaryCache::ensure_(const char *group_id) {
  if (auto *existing = this->find_mutable_(group_id))
    return existing;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied)
      return &summary;
  }
  return nullptr;
}

}  // namespace radar_api_server
}  // namespace esphome
