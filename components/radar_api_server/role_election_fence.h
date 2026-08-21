#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace radar_api_server {

template <std::size_t MaxMembers> class BoundedRoleElectionView {
 public:
  static constexpr std::size_t MAX_MEMBERS = MaxMembers;

  void add(const char *participant_id, uint64_t qualifier = 0,
           bool credentials_complete = true) {
    if (participant_id == nullptr || participant_id[0] == '\0' ||
        this->size_ >= this->members_.size())
      return;
    for (std::size_t index = 0; index < this->size_; ++index) {
      if (std::strcmp(this->members_[index].participant_id, participant_id) == 0)
        return;
    }
    this->members_[this->size_++] = {
        participant_id, qualifier, credentials_complete};
  }

  uint32_t digest(const char *scope) const {
    return this->digest_(scope, true);
  }

  uint32_t contract_digest(const char *scope) const {
    return this->digest_(scope, false);
  }

  uint32_t winner_contract_digest(const char *scope,
                                  const char *selected_winner) const {
    uint32_t hash = this->digest_(scope, false);
    append_context_(&hash, "winner", selected_winner);
    return hash == 0 ? 1U : hash;
  }

 private:
  uint32_t digest_(const char *scope, bool include_credentials) const {
    auto sorted = this->members_;
    for (std::size_t index = 1; index < this->size_; ++index) {
      const Item value = sorted[index];
      std::size_t insertion = index;
      while (insertion > 0 &&
             std::strcmp(value.participant_id,
                         sorted[insertion - 1].participant_id) < 0) {
        sorted[insertion] = sorted[insertion - 1];
        --insertion;
      }
      sorted[insertion] = value;
    }

    uint32_t hash = 2166136261U;
    append_(&hash, scope != nullptr ? scope : "");
    append_byte_(&hash, 0);
    for (std::size_t index = 0; index < this->size_; ++index) {
      append_(&hash, sorted[index].participant_id);
      append_byte_(&hash, 0);
      for (uint8_t shift = 0; shift < 8; ++shift)
        append_byte_(&hash, static_cast<uint8_t>(
                                sorted[index].qualifier >> (shift * 8U)));
      if (include_credentials)
        append_byte_(&hash, sorted[index].credentials_complete ? 1U : 0U);
    }
    return hash == 0 ? 1U : hash;
  }
  static void append_(uint32_t *hash, const char *value) {
    for (const auto *cursor = value; *cursor != '\0'; ++cursor)
      append_byte_(hash, static_cast<uint8_t>(*cursor));
  }

  static void append_context_(uint32_t *hash, const char *name,
                              const char *value) {
    append_byte_(hash, 0xFFU);
    append_(hash, name);
    append_byte_(hash, 0);
    append_(hash, value != nullptr ? value : "");
    append_byte_(hash, 0);
  }

  static void append_byte_(uint32_t *hash, uint8_t value) {
    *hash ^= value;
    *hash *= 16777619U;
  }

  struct Item {
    const char *participant_id{nullptr};
    uint64_t qualifier{0};
    bool credentials_complete{false};
  };

  std::array<Item, MAX_MEMBERS> members_{};
  std::size_t size_{0};
};

using RoleElectionView = BoundedRoleElectionView<7>;
using SiteRoleElectionView = BoundedRoleElectionView<20>;

struct RoleElectionTransitionViews {
  // One logical configuration update can expose several short-lived views as
  // the roster and Directory reach peers independently. Keep only a bounded
  // history; callers still require the same incumbent, term, and quorum.
  static constexpr std::size_t MAX_VIEWS = 3;

  struct View {
    uint32_t modern{0};
    uint32_t compatibility{0};
    uint32_t contract{0};
    uint32_t legacy{0};
  };

  void capture(uint32_t modern_digest, uint32_t compatibility_digest,
               uint32_t contract_digest, uint32_t legacy_digest) {
    if (modern_digest == 0 && compatibility_digest == 0 &&
        contract_digest == 0 && legacy_digest == 0)
      return;
    const View view{modern_digest, compatibility_digest, contract_digest,
                    legacy_digest};
    for (std::size_t index = 0; index < this->size_; ++index) {
      if (this->views_[index].modern == view.modern &&
          this->views_[index].compatibility == view.compatibility &&
          this->views_[index].contract == view.contract &&
          this->views_[index].legacy == view.legacy)
        return;
    }
    if (this->size_ < this->views_.size()) {
      this->views_[this->size_++] = view;
      return;
    }
    for (std::size_t index = 1; index < this->views_.size(); ++index)
      this->views_[index - 1] = this->views_[index];
    this->views_.back() = view;
  }

  void reset() { *this = {}; }

  bool active() const { return this->size_ != 0; }

  static uint32_t digest_for(const View &view, uint8_t contract_version) {
    if (contract_version >= 3)
      return view.modern;
    if (contract_version == 2)
      return view.compatibility;
    if (contract_version == 1)
      return view.contract;
    return view.legacy;
  }

  bool matches(uint8_t contract_version, uint32_t digest) const {
    for (std::size_t index = 0; index < this->size_; ++index) {
      const uint32_t expected =
          digest_for(this->views_[index], contract_version);
      if (expected != 0 && digest == expected)
        return true;
    }
    return false;
  }

 private:
  std::array<View, MAX_VIEWS> views_{};
  std::size_t size_{0};
};

template <std::size_t MaxDigests> class BoundedRoleElectionDigestVotes {
 public:
  void add(uint8_t contract_version, uint32_t digest) {
    if (digest == 0)
      return;
    for (std::size_t index = 0; index < this->size_; ++index) {
      auto &entry = this->entries_[index];
      if (entry.contract_version == contract_version &&
          entry.digest == digest) {
        if (entry.count != UINT8_MAX)
          ++entry.count;
        return;
      }
    }
    if (this->size_ < this->entries_.size())
      this->entries_[this->size_++] = {digest, contract_version, 1};
  }

  uint8_t strongest() const {
    uint8_t strongest = 0;
    for (std::size_t index = 0; index < this->size_; ++index) {
      if (this->entries_[index].count > strongest)
        strongest = this->entries_[index].count;
    }
    return strongest;
  }

 private:
  struct Entry {
    uint32_t digest{0};
    uint8_t contract_version{0};
    uint8_t count{0};
  };

  std::array<Entry, MaxDigests> entries_{};
  std::size_t size_{0};
};

struct RoleTermDecision {
  uint32_t term{0};
  bool authority_ready{false};
};

inline uint32_t next_role_term(uint32_t value) {
  return value == UINT32_MAX ? UINT32_MAX : value + 1U;
}

inline bool role_id_equal(const char *left, const char *right) {
  return left != nullptr && right != nullptr && left[0] != '\0' &&
         right[0] != '\0' && std::strcmp(left, right) == 0;
}

inline uint8_t preserve_incumbent_transition_endorsements(
    bool prior_quorum_available, const char *prior_leader,
    uint32_t prior_term, uint32_t observed_term, const char *winner,
    uint8_t current_endorsements, uint8_t prior_view_endorsements,
    uint8_t required_endorsements) {
  if (!prior_quorum_available || prior_term == 0 ||
      !role_id_equal(prior_leader, winner) || observed_term > prior_term ||
      current_endorsements >= required_endorsements ||
      prior_view_endorsements == 0)
    return current_endorsements;

  const uint16_t combined = static_cast<uint16_t>(current_endorsements) +
                            prior_view_endorsements;
  return combined >= required_endorsements
             ? static_cast<uint8_t>(combined > UINT8_MAX ? UINT8_MAX
                                                         : combined)
             : current_endorsements;
}

inline RoleTermDecision decide_role_term(
    uint32_t current_term, const char *current_leader,
    uint32_t suspended_term, const char *suspended_leader,
    const char *winner, bool winner_is_local, uint32_t winner_term,
    bool winner_term_admitted, uint32_t observed_term) {
  const bool same_current = role_id_equal(current_leader, winner);
  const bool same_suspended =
      (current_leader == nullptr || current_leader[0] == '\0') &&
      role_id_equal(suspended_leader, winner);
  uint32_t prior_term = current_term;
  if (suspended_term > prior_term)
    prior_term = suspended_term;

  if (winner_is_local) {
    if (same_current && observed_term <= current_term)
      return {current_term == 0 ? 1U : current_term, true};
    if (same_suspended && observed_term <= suspended_term)
      return {suspended_term == 0 ? 1U : suspended_term, true};
    if ((current_leader == nullptr || current_leader[0] == '\0') &&
        (suspended_leader == nullptr || suspended_leader[0] == '\0') &&
        observed_term <= current_term)
      return {current_term == 0 ? 1U : next_role_term(current_term), true};
    if (observed_term > prior_term)
      prior_term = observed_term;
    return {next_role_term(prior_term), true};
  }

  if (!winner_term_admitted)
    return {prior_term == 0 ? 1U : prior_term, false};
  if (same_current || same_suspended)
    return winner_term >= prior_term
               ? RoleTermDecision{winner_term, true}
               : RoleTermDecision{prior_term, false};
  if ((current_leader == nullptr || current_leader[0] == '\0') &&
      (suspended_leader == nullptr || suspended_leader[0] == '\0'))
    return winner_term >= current_term
               ? RoleTermDecision{winner_term, true}
               : RoleTermDecision{current_term, false};
  return winner_term > prior_term
             ? RoleTermDecision{winner_term, true}
             : RoleTermDecision{prior_term, false};
}

}  // namespace radar_api_server
}  // namespace esphome
