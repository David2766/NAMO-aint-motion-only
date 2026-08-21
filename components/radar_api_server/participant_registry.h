#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class ParticipantType : uint8_t {
  NAMO_NODE = 0,
  SATELLITE,
};

enum class ParticipantTrust : uint8_t {
  UNKNOWN = 0,
  OFFICIAL_VERIFIED,
  COMMUNITY_UNVERIFIED,
  SATELLITE,
};

enum class ParticipantMembership : uint8_t {
  QUARANTINED = 0,
  ACTIVE,
  REVOKED,
};

enum class ParticipantRegistryResult : uint8_t {
  OK = 0,
  INVALID_ID,
  ALREADY_EXISTS,
  NOT_FOUND,
  FULL,
  INCOMPATIBLE_TRUST,
  INVALID_STATE,
};

struct ParticipantRecord {
  static constexpr std::size_t MAX_ID_LENGTH = 64;

  bool occupied{false};
  char participant_id[MAX_ID_LENGTH + 1]{};
  ParticipantType type{ParticipantType::NAMO_NODE};
  ParticipantTrust trust{ParticipantTrust::UNKNOWN};
  ParticipantMembership membership{ParticipantMembership::QUARANTINED};
  bool leader_eligible{false};
};

class ParticipantRegistry {
 public:
  // A Site has one local node plus at most nineteen remote participants.
  static constexpr std::size_t MAX_PARTICIPANTS = 20;
  static constexpr std::size_t LOCAL_PARTICIPANT_COUNT = 1;
  static constexpr std::size_t MAX_REMOTE_PARTICIPANTS =
      MAX_PARTICIPANTS - LOCAL_PARTICIPANT_COUNT;

  ParticipantRegistryResult discover(const char *participant_id, ParticipantType type);
  ParticipantRegistryResult classify(const char *participant_id, ParticipantTrust trust);
  ParticipantRegistryResult activate(const char *participant_id);
  ParticipantRegistryResult set_community_leader_eligible(const char *participant_id, bool eligible);
  ParticipantRegistryResult quarantine(const char *participant_id);
  ParticipantRegistryResult revoke(const char *participant_id);
  ParticipantRegistryResult forget(const char *participant_id);
  void clear();

  const ParticipantRecord *find(const char *participant_id) const;
  const ParticipantRecord *record_at(std::size_t slot) const;
  std::size_t size() const;
  std::size_t connected_count() const;
  std::size_t active_count() const;
  std::size_t site_size() const { return LOCAL_PARTICIPANT_COUNT + this->size(); }
  std::size_t site_active_count() const {
    return LOCAL_PARTICIPANT_COUNT + this->active_count();
  }

 private:
  std::array<ParticipantRecord, MAX_REMOTE_PARTICIPANTS> entries_{};

  ParticipantRecord *find_mutable_(const char *participant_id);
  static bool valid_id_(const char *participant_id);
  static bool compatible_trust_(ParticipantType type, ParticipantTrust trust);
  static void copy_id_(char *target, const char *source);
};

static_assert(std::is_trivially_copyable<ParticipantRecord>::value,
              "participant records must not own heap-backed state");
static_assert(std::is_trivially_copyable<ParticipantRegistry>::value,
              "participant registry must not own heap-backed state");
static_assert(sizeof(ParticipantRegistry) <= 2048,
              "participant registry memory exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
