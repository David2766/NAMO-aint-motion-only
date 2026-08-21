#include "participant_registry.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

ParticipantRegistryResult ParticipantRegistry::discover(const char *participant_id,
                                                          ParticipantType type) {
  if (!valid_id_(participant_id))
    return ParticipantRegistryResult::INVALID_ID;
  if (this->find(participant_id) != nullptr)
    return ParticipantRegistryResult::ALREADY_EXISTS;

  for (auto &entry : this->entries_) {
    if (entry.occupied)
      continue;

    entry = ParticipantRecord{};
    entry.occupied = true;
    copy_id_(entry.participant_id, participant_id);
    entry.type = type;
    return ParticipantRegistryResult::OK;
  }
  return ParticipantRegistryResult::FULL;
}

ParticipantRegistryResult ParticipantRegistry::classify(const char *participant_id,
                                                          ParticipantTrust trust) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;
  if (entry->membership != ParticipantMembership::QUARANTINED)
    return ParticipantRegistryResult::INVALID_STATE;
  if (!compatible_trust_(entry->type, trust))
    return ParticipantRegistryResult::INCOMPATIBLE_TRUST;

  entry->trust = trust;
  entry->leader_eligible = false;
  return ParticipantRegistryResult::OK;
}

ParticipantRegistryResult ParticipantRegistry::activate(const char *participant_id) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;
  if (entry->membership != ParticipantMembership::QUARANTINED ||
      entry->trust == ParticipantTrust::UNKNOWN)
    return ParticipantRegistryResult::INVALID_STATE;
  if (!compatible_trust_(entry->type, entry->trust))
    return ParticipantRegistryResult::INCOMPATIBLE_TRUST;

  entry->membership = ParticipantMembership::ACTIVE;
  entry->leader_eligible =
      entry->type == ParticipantType::NAMO_NODE &&
      entry->trust == ParticipantTrust::OFFICIAL_VERIFIED;
  return ParticipantRegistryResult::OK;
}

ParticipantRegistryResult ParticipantRegistry::set_community_leader_eligible(
    const char *participant_id, bool eligible) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;
  if (entry->membership != ParticipantMembership::ACTIVE ||
      entry->type != ParticipantType::NAMO_NODE ||
      entry->trust != ParticipantTrust::COMMUNITY_UNVERIFIED)
    return ParticipantRegistryResult::INVALID_STATE;

  entry->leader_eligible = eligible;
  return ParticipantRegistryResult::OK;
}

ParticipantRegistryResult ParticipantRegistry::quarantine(const char *participant_id) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;
  if (entry->membership == ParticipantMembership::REVOKED)
    return ParticipantRegistryResult::INVALID_STATE;

  entry->membership = ParticipantMembership::QUARANTINED;
  entry->leader_eligible = false;
  return ParticipantRegistryResult::OK;
}

ParticipantRegistryResult ParticipantRegistry::revoke(const char *participant_id) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;

  entry->membership = ParticipantMembership::REVOKED;
  entry->leader_eligible = false;
  return ParticipantRegistryResult::OK;
}

ParticipantRegistryResult ParticipantRegistry::forget(const char *participant_id) {
  ParticipantRecord *entry = this->find_mutable_(participant_id);
  if (entry == nullptr)
    return ParticipantRegistryResult::NOT_FOUND;
  if (entry->membership == ParticipantMembership::ACTIVE)
    return ParticipantRegistryResult::INVALID_STATE;

  *entry = ParticipantRecord{};
  return ParticipantRegistryResult::OK;
}

void ParticipantRegistry::clear() {
  for (auto &entry : this->entries_)
    entry = ParticipantRecord{};
}

const ParticipantRecord *ParticipantRegistry::find(const char *participant_id) const {
  if (!valid_id_(participant_id))
    return nullptr;
  for (const auto &entry : this->entries_) {
    if (entry.occupied && std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

const ParticipantRecord *ParticipantRegistry::record_at(std::size_t slot) const {
  if (slot >= this->entries_.size() || !this->entries_[slot].occupied)
    return nullptr;
  return &this->entries_[slot];
}

std::size_t ParticipantRegistry::size() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_)
    count += entry.occupied ? 1U : 0U;
  return count;
}

std::size_t ParticipantRegistry::connected_count() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_)
    count += entry.occupied &&
                     entry.membership != ParticipantMembership::REVOKED
                 ? 1U
                 : 0U;
  return count;
}

std::size_t ParticipantRegistry::active_count() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_)
    count += entry.occupied && entry.membership == ParticipantMembership::ACTIVE ? 1U : 0U;
  return count;
}

ParticipantRecord *ParticipantRegistry::find_mutable_(const char *participant_id) {
  if (!valid_id_(participant_id))
    return nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied && std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

bool ParticipantRegistry::valid_id_(const char *participant_id) {
  if (participant_id == nullptr || participant_id[0] == '\0')
    return false;

  std::size_t length = 0;
  for (const char *cursor = participant_id; *cursor != '\0'; ++cursor) {
    if (++length > ParticipantRecord::MAX_ID_LENGTH)
      return false;
    const char value = *cursor;
    const bool alpha_numeric = (value >= 'a' && value <= 'z') ||
                               (value >= 'A' && value <= 'Z') ||
                               (value >= '0' && value <= '9');
    if (!alpha_numeric && (cursor == participant_id ||
                           (value != '.' && value != '_' && value != ':' && value != '-')))
      return false;
  }
  return true;
}

bool ParticipantRegistry::compatible_trust_(ParticipantType type, ParticipantTrust trust) {
  if (trust == ParticipantTrust::UNKNOWN)
    return true;
  if (type == ParticipantType::NAMO_NODE)
    return trust == ParticipantTrust::OFFICIAL_VERIFIED ||
           trust == ParticipantTrust::COMMUNITY_UNVERIFIED;
  return trust == ParticipantTrust::SATELLITE;
}

void ParticipantRegistry::copy_id_(char *target, const char *source) {
  std::strncpy(target, source, ParticipantRecord::MAX_ID_LENGTH);
  target[ParticipantRecord::MAX_ID_LENGTH] = '\0';
}

}  // namespace radar_api_server
}  // namespace esphome
