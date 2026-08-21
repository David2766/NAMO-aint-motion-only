#include "site_group_summary.h"

#include <cstring>
#include <limits>

namespace esphome {
namespace radar_api_server {

namespace {

bool same_text(const char *left, const char *right) {
  return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

}  // namespace

SiteGroupSummaryResult SiteGroupSummaryCache::accept(
    const SiteDirectorySnapshot &directory,
    const SiteGroupSummaryReport &report, uint32_t now_ms) {
  if (!valid_text_(report.participant_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(report.group_id,
                   ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      report.assignment_revision == 0 || report.term == 0 ||
      report.lease_ms < GroupCoordinator::MIN_LEASE_MS ||
      report.lease_ms > GroupCoordinator::MAX_LEASE_MS ||
      report.participants_expected == 0 ||
      report.participants_expected >
          SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ||
      report.participants_online > report.participants_expected) {
    this->rejected_++;
    return SiteGroupSummaryResult::INVALID_ARGUMENT;
  }
  if (!directory.committed) {
    this->rejected_++;
    return SiteGroupSummaryResult::DIRECTORY_NOT_READY;
  }

  const auto *group = directory.find_group(report.group_id);
  if (group == nullptr) {
    this->rejected_++;
    return SiteGroupSummaryResult::UNKNOWN_GROUP;
  }
  if (!same_text(group->leader_node_id, report.participant_id)) {
    this->rejected_++;
    return SiteGroupSummaryResult::LEADER_MISMATCH;
  }
  if (group->assignment_revision != report.assignment_revision) {
    this->rejected_++;
    return SiteGroupSummaryResult::REVISION_MISMATCH;
  }
  if (group->member_count != report.participants_expected) {
    this->rejected_++;
    return SiteGroupSummaryResult::MEMBER_COUNT_MISMATCH;
  }
  auto *summary = this->ensure_(report.group_id);
  if (summary == nullptr) {
    this->rejected_++;
    return SiteGroupSummaryResult::FULL;
  }
  summary->occupied = true;
  copy_(summary->group_id, sizeof(summary->group_id), group->group_id);
  copy_(summary->floorplan_id, sizeof(summary->floorplan_id),
        group->floorplan_id);
  copy_(summary->leader_node_id, sizeof(summary->leader_node_id),
        group->leader_node_id);
  summary->assignment_revision = report.assignment_revision;
  summary->term = report.term;
  summary->lease_expires_at_ms = now_ms + report.lease_ms;
  summary->participants_expected = report.participants_expected;
  summary->participants_online = report.participants_online;
  this->generation_++;
  return SiteGroupSummaryResult::OK;
}

SiteGroupSummaryResult SiteGroupSummaryCache::accept_discovered(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *directory,
    const char *expected_leader_node_id, const char *floorplan_id,
    const SiteGroupSummaryReport &report, uint32_t now_ms) {
  if (!valid_text_(expected_leader_node_id,
                   ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(floorplan_id, ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(report.participant_id,
                   ParticipantRecord::MAX_ID_LENGTH) ||
      !valid_text_(report.group_id,
                   ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) ||
      report.assignment_revision == 0 || report.term == 0 ||
      report.lease_ms < GroupCoordinator::MIN_LEASE_MS ||
      report.lease_ms > GroupCoordinator::MAX_LEASE_MS ||
      report.participants_expected == 0 ||
      report.participants_expected >
          SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ||
      report.participants_online > report.participants_expected) {
    ++this->rejected_;
    return SiteGroupSummaryResult::INVALID_ARGUMENT;
  }
  if (!same_text(expected_leader_node_id, report.participant_id)) {
    ++this->rejected_;
    return SiteGroupSummaryResult::LEADER_MISMATCH;
  }
  const auto *leader = configuration.find(report.participant_id);
  if (leader == nullptr ||
      leader->membership != ParticipantMembership::ACTIVE ||
      leader->type != ParticipantType::NAMO_NODE ||
      !leader->leader_eligible || !leader->spatial_profile.committed ||
      !same_text(leader->spatial_profile.group_id, report.group_id)) {
    ++this->rejected_;
    return SiteGroupSummaryResult::LEADER_MISMATCH;
  }

  uint8_t member_count = 0;
  for (const auto &member : configuration.members) {
    if (member.occupied &&
        member.membership == ParticipantMembership::ACTIVE &&
        member.spatial_profile.committed &&
        same_text(member.spatial_profile.group_id, report.group_id))
      ++member_count;
  }
  if (member_count != report.participants_expected) {
    ++this->rejected_;
    return SiteGroupSummaryResult::MEMBER_COUNT_MISMATCH;
  }
  const auto *directory_group =
      directory != nullptr && directory->committed
          ? directory->find_group(report.group_id)
          : nullptr;
  const SiteGroupSummaryState *newest = nullptr;
  for (const auto &summary : this->summaries_) {
    if (!summary.occupied)
      continue;
    if (same_text(summary.leader_node_id, report.participant_id) &&
        !same_text(summary.group_id, report.group_id)) {
      ++this->rejected_;
      return SiteGroupSummaryResult::LEADER_MISMATCH;
    }
    if (same_text(summary.group_id, report.group_id))
      newest = &summary;
  }

  uint32_t assignment_revision = 1;
  const char *assigned_floorplan = nullptr;
  const char *assigned_leader = nullptr;
  uint8_t assigned_members = 0;
  if (directory_group != nullptr) {
    assignment_revision = directory_group->assignment_revision;
    assigned_floorplan = directory_group->floorplan_id;
    assigned_leader = directory_group->leader_node_id;
    assigned_members = directory_group->member_count;
  }
  if (newest != nullptr &&
      newest->assignment_revision > assignment_revision) {
    assignment_revision = newest->assignment_revision;
    assigned_floorplan = newest->floorplan_id;
    assigned_leader = newest->leader_node_id;
    assigned_members = newest->participants_expected;
  }
  const bool assignment_matches =
      assigned_floorplan != nullptr && assigned_leader != nullptr &&
      same_text(assigned_floorplan, floorplan_id) &&
      same_text(assigned_leader, report.participant_id) &&
      assigned_members == report.participants_expected;
  if (!assignment_matches &&
      assignment_revision == std::numeric_limits<uint32_t>::max()) {
    ++this->rejected_;
    return SiteGroupSummaryResult::REVISION_MISMATCH;
  }
  if (!assignment_matches &&
      (directory_group != nullptr || newest != nullptr))
    ++assignment_revision;

  auto *summary = this->ensure_(report.group_id);
  if (summary == nullptr) {
    ++this->rejected_;
    return SiteGroupSummaryResult::FULL;
  }
  summary->occupied = true;
  copy_(summary->group_id, sizeof(summary->group_id), report.group_id);
  copy_(summary->floorplan_id, sizeof(summary->floorplan_id), floorplan_id);
  copy_(summary->leader_node_id, sizeof(summary->leader_node_id),
        report.participant_id);
  summary->assignment_revision = assignment_revision;
  summary->term = report.term;
  summary->lease_expires_at_ms = now_ms + report.lease_ms;
  summary->participants_expected = report.participants_expected;
  summary->participants_online = report.participants_online;
  ++this->generation_;
  return SiteGroupSummaryResult::OK;
}

void SiteGroupSummaryCache::reconcile(
    const SiteDirectorySnapshot *directory, bool local_authority,
    uint32_t now_ms) {
  if (!local_authority || directory == nullptr || !directory->committed) {
    this->clear();
    return;
  }

  bool changed = false;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied)
      continue;
    const auto *group = directory->find_group(summary.group_id);
    const bool expired =
        time_reached_(now_ms, summary.lease_expires_at_ms);
    if (!expired) {
      if (group == nullptr)
        continue;
      if (same_text(group->floorplan_id, summary.floorplan_id) &&
          same_text(group->leader_node_id, summary.leader_node_id) &&
          group->assignment_revision == summary.assignment_revision &&
          group->member_count == summary.participants_expected)
        continue;
      if (summary.assignment_revision > group->assignment_revision)
        continue;
    }
    if (expired)
      this->expired_++;
    summary = {};
    changed = true;
  }
  if (changed)
    this->generation_++;
}

void SiteGroupSummaryCache::clear() {
  bool changed = false;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied)
      continue;
    summary = {};
    changed = true;
  }
  if (changed)
    this->generation_++;
}

const SiteGroupSummaryState *SiteGroupSummaryCache::find(
    const char *group_id) const {
  if (group_id == nullptr)
    return nullptr;
  for (const auto &summary : this->summaries_) {
    if (summary.occupied && same_text(summary.group_id, group_id))
      return &summary;
  }
  return nullptr;
}

const SiteGroupSummaryState *SiteGroupSummaryCache::summary_at(
    std::size_t slot) const {
  return slot < this->summaries_.size() && this->summaries_[slot].occupied
             ? &this->summaries_[slot]
             : nullptr;
}

std::size_t SiteGroupSummaryCache::size() const {
  std::size_t count = 0;
  for (const auto &summary : this->summaries_)
    count += summary.occupied ? 1U : 0U;
  return count;
}

uint32_t SiteGroupSummaryCache::lease_remaining_ms(
    const SiteGroupSummaryState &state, uint32_t now_ms) {
  if (time_reached_(now_ms, state.lease_expires_at_ms))
    return 0;
  return state.lease_expires_at_ms - now_ms;
}

SiteGroupSummaryState *SiteGroupSummaryCache::find_mutable_(
    const char *group_id) {
  return const_cast<SiteGroupSummaryState *>(this->find(group_id));
}

SiteGroupSummaryState *SiteGroupSummaryCache::ensure_(const char *group_id) {
  if (auto *existing = this->find_mutable_(group_id))
    return existing;
  for (auto &summary : this->summaries_) {
    if (!summary.occupied)
      return &summary;
  }
  return nullptr;
}

bool SiteGroupSummaryCache::time_reached_(uint32_t now_ms,
                                          uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool SiteGroupSummaryCache::valid_text_(const char *value,
                                        std::size_t maximum) {
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

void SiteGroupSummaryCache::copy_(char *target, std::size_t capacity,
                                  const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

const char *site_group_summary_result_to_string(
    SiteGroupSummaryResult result) {
  switch (result) {
    case SiteGroupSummaryResult::OK:
      return "ok";
    case SiteGroupSummaryResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case SiteGroupSummaryResult::DIRECTORY_NOT_READY:
      return "directory_not_ready";
    case SiteGroupSummaryResult::UNKNOWN_GROUP:
      return "unknown_group";
    case SiteGroupSummaryResult::LEADER_MISMATCH:
      return "leader_mismatch";
    case SiteGroupSummaryResult::REVISION_MISMATCH:
      return "revision_mismatch";
    case SiteGroupSummaryResult::MEMBER_COUNT_MISMATCH:
      return "member_count_mismatch";
    case SiteGroupSummaryResult::FULL:
      return "full";
  }
  return "invalid_argument";
}

}  // namespace radar_api_server
}  // namespace esphome
