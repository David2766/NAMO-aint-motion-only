#pragma once

#include "participant_registry.h"
#include "site_configuration.h"

#include <cstddef>
#include <cstring>

namespace esphome {
namespace radar_api_server {

struct ParticipantProjection {
  const SiteConfigurationMember *canonical_member{nullptr};
  const ParticipantRecord *direct_pair{nullptr};

  const char *participant_id() const {
    return this->canonical_member != nullptr
               ? this->canonical_member->participant_id
               : (this->direct_pair != nullptr
                      ? this->direct_pair->participant_id
                      : nullptr);
  }

  ParticipantType type() const {
    return this->canonical_member != nullptr
               ? this->canonical_member->type
               : this->direct_pair->type;
  }

  ParticipantTrust trust() const {
    return this->canonical_member != nullptr
               ? this->canonical_member->trust
               : this->direct_pair->trust;
  }

  ParticipantMembership membership() const {
    return this->canonical_member != nullptr
               ? this->canonical_member->membership
               : this->direct_pair->membership;
  }

  const ParticipantMembership *canonical_membership() const {
    return this->canonical_member != nullptr
               ? &this->canonical_member->membership
               : nullptr;
  }

  const ParticipantMembership *direct_membership() const {
    return this->direct_pair != nullptr ? &this->direct_pair->membership
                                        : nullptr;
  }

  bool leader_eligible() const {
    return this->canonical_member != nullptr
               ? this->canonical_member->leader_eligible
               : this->direct_pair->leader_eligible;
  }

  bool canonical() const { return this->canonical_member != nullptr; }
  bool directly_paired() const { return this->direct_pair != nullptr; }
};

inline bool participant_projection_at(
    const char *local_participant_id,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantRegistry &direct_pairs, std::size_t index,
    ParticipantProjection *projection) {
  if (local_participant_id == nullptr || projection == nullptr)
    return false;
  if (index >= ParticipantRegistry::MAX_REMOTE_PARTICIPANTS)
    return false;

  *projection = {};
  std::size_t current = 0;
  if (configuration != nullptr && configuration->roster_committed) {
    for (const auto &member : configuration->members) {
      if (!member.occupied ||
          std::strcmp(member.participant_id, local_participant_id) == 0)
        continue;
      if (current++ != index)
        continue;
      projection->canonical_member = &member;
      projection->direct_pair = direct_pairs.find(member.participant_id);
      return true;
    }
  }

  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *direct = direct_pairs.record_at(slot);
    if (direct == nullptr ||
        (configuration != nullptr && configuration->roster_committed &&
         configuration->find(direct->participant_id) != nullptr))
      continue;
    if (current++ != index)
      continue;
    projection->direct_pair = direct;
    return true;
  }
  return false;
}

inline std::size_t participant_projection_count(
    const char *local_participant_id,
    const SiteConfigurationSnapshot *configuration,
    const ParticipantRegistry &direct_pairs) {
  std::size_t count = 0;
  ParticipantProjection projection{};
  while (participant_projection_at(local_participant_id, configuration,
                                   direct_pairs, count, &projection))
    ++count;
  return count;
}

}  // namespace radar_api_server
}  // namespace esphome
