#include "group_roster.h"
#include "site_directory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "group_roster_tests: %s\n", message);
  std::exit(1);
}

class FakeBackend : public GroupRosterStorageBackend {
 public:
  FakeBackend() {
    for (auto &slot : this->slots)
      slot.bytes.fill(0xFF);
    this->commit.bytes.fill(0xFF);
  }

  bool load_group_roster_slot(uint8_t slot,
                              GroupRosterStorageBlob *blob) override {
    if (slot > 1 || blob == nullptr || this->fail_load)
      return false;
    *blob = this->slots[slot];
    return true;
  }

  bool save_group_roster_slot(
      uint8_t slot, const GroupRosterStorageBlob &blob) override {
    if (slot > 1 || this->fail_slot_write)
      return false;
    this->slots[slot] = blob;
    return true;
  }

  bool load_group_roster_commit(GroupRosterCommitBlob *blob) override {
    if (blob == nullptr || this->fail_load)
      return false;
    *blob = this->commit;
    return true;
  }

  bool save_group_roster_commit(
      const GroupRosterCommitBlob &blob) override {
    if (this->fail_commit_write)
      return false;
    this->commit = blob;
    return true;
  }

  bool sync() override { return !this->fail_sync; }

  GroupRosterStorageBlob slots[2]{};
  GroupRosterCommitBlob commit{};
  bool fail_load{false};
  bool fail_slot_write{false};
  bool fail_commit_write{false};
  bool fail_sync{false};
};

SpatialTransform2D transform_for(std::size_t index) {
  SpatialTransform2D transform{};
  transform.origin_x_mm = static_cast<int32_t>(index * 500);
  transform.origin_y_mm = static_cast<int32_t>(index * 250);
  transform.rotation_cdeg = static_cast<int16_t>(index * 100);
  transform.residual_mm = 20;
  transform.sample_count = 1;
  transform.valid = true;
  return transform;
}

SiteConfigurationSnapshot configuration_with_group(std::size_t count = 7) {
  SiteConfigurationSnapshot configuration{};
  std::strcpy(configuration.site_id, "site-home");
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-00");
  configuration.authority_epoch = 4;
  configuration.generation = 9;
  configuration.roster_revision = 12;
  configuration.roster_committed = true;
  configuration.floorplan.committed = true;
  std::strcpy(configuration.floorplan.floorplan_id, "floorplan-main");
  configuration.floorplan.geometry_version = 5;
  configuration.floorplan.config_hash = 101;
  configuration.floorplan.image_hash = 202;
  for (std::size_t index = 0; index < count; ++index) {
    auto &member = configuration.members[index];
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-%02u", static_cast<unsigned>(index));
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = static_cast<uint32_t>(index + 1);
    member.spatial_profile.calibration_version = 3;
    member.spatial_profile.transform = transform_for(index);
  }
  return configuration;
}

std::size_t mask_member_count(uint8_t mask) {
  std::size_t count = 0;
  for (uint8_t index = 0; index < 7; ++index)
    count += (mask & (1U << index)) != 0 ? 1U : 0U;
  return count;
}

SiteConfigurationSnapshot configuration_for_mask(uint8_t mask,
                                                 uint32_t revision) {
  auto configuration = configuration_with_group(0);
  configuration.generation = revision;
  configuration.roster_revision = revision;
  std::size_t slot = 0;
  for (uint8_t index = 0; index < 7; ++index) {
    if ((mask & (1U << index)) == 0)
      continue;
    auto &member = configuration.members[slot++];
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-%02u", static_cast<unsigned>(index));
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile.committed = true;
    std::strcpy(member.spatial_profile.space_id, "room-main");
    std::strcpy(member.spatial_profile.group_id, "group-main");
    member.spatial_profile.pose_version = static_cast<uint32_t>(index + 1);
    member.spatial_profile.calibration_version = 3;
    member.spatial_profile.transform = transform_for(index);
  }
  const auto *first = configuration.members.data();
  std::strcpy(configuration.configuration_owner_node_id,
              first->participant_id);
  return configuration;
}

GroupRosterSnapshot populated_roster() {
  const auto configuration = configuration_with_group();
  GroupRosterSnapshot roster{};
  require(prepare_group_roster_from_site_configuration(
              configuration, "group-main", "namo-node-00", &roster) ==
              GroupRosterResult::OK,
          "valid seven-member configuration did not produce a Group roster");
  return roster;
}

void seven_member_roster_is_bounded_and_round_trips() {
  const auto roster = populated_roster();
  require(roster.member_count() == 7 &&
              roster.find("namo-node-06") != nullptr &&
              valid_group_roster(roster),
          "seven-member Group roster lost a member");

  GroupRosterStorageBlob encoded{};
  SiteConfigurationSnapshot codec_workspace{};
  std::size_t encoded_size = 0;
  require(GroupRosterCodec::encode(roster, encoded.bytes.data(),
                                   encoded.bytes.size(), &encoded_size,
                                   &codec_workspace) ==
              GroupRosterCodecResult::OK &&
              encoded_size > 0,
          "Group roster encoding failed");
  GroupRosterSnapshot decoded{};
  require(GroupRosterCodec::decode(encoded.bytes.data(), encoded.bytes.size(),
                                   &decoded, &codec_workspace) ==
              GroupRosterCodecResult::OK &&
              same_group_roster_payload(roster, decoded),
          "Group roster codec changed its semantic payload");

  const auto overflow = configuration_with_group(8);
  GroupRosterSnapshot unchanged = roster;
  require(prepare_group_roster_from_site_configuration(
              overflow, "group-main", "namo-node-00", &unchanged) ==
              GroupRosterResult::FULL &&
              unchanged.member_count() == 0,
          "an eighth member did not fail closed without publishing a roster");
}

void legacy_configuration_migrates_once() {
  FakeBackend backend;
  GroupRosterStore store;
  GroupRosterSnapshot roster{};
  const auto configuration = configuration_with_group();
  require(store.load_or_migrate(backend, configuration, "group-main",
                                "namo-node-00", &roster) ==
              GroupRosterStoreResult::MIGRATED &&
              roster.generation == 1 && roster.member_count() == 7,
          "legacy Site roster did not migrate atomically");

  GroupRosterStore rebooted_store;
  GroupRosterSnapshot rebooted{};
  require(rebooted_store.load_or_migrate(
              backend, configuration, "group-main", "namo-node-00",
              &rebooted) == GroupRosterStoreResult::LOADED &&
              same_group_roster_payload(roster, rebooted),
          "a migrated Group roster was rebuilt instead of loaded");
}

void empty_migration_retries_when_configuration_becomes_ready() {
  FakeBackend backend;
  GroupRosterStore store;
  GroupRosterSnapshot roster{};
  auto configuration = configuration_with_group();
  configuration.floorplan.committed = false;
  require(store.load_or_migrate(backend, configuration, "group-main",
                                "namo-node-00", &roster) ==
              GroupRosterStoreResult::EMPTY,
          "an incomplete configuration should leave Group storage empty");

  configuration.floorplan.committed = true;
  require(store.load_or_migrate(backend, configuration, "group-main",
                                "namo-node-00", &roster) ==
              GroupRosterStoreResult::MIGRATED &&
              valid_group_roster(roster) && roster.member_count() == 7,
          "an empty Group migration did not recover after configuration "
          "became ready");
}

void leader_handoff_is_atomic_across_reboot() {
  FakeBackend backend;
  GroupRosterStore store;
  GroupRosterSnapshot current = populated_roster();
  require(store.create(backend, current, &current) ==
              GroupRosterStoreResult::CREATED,
          "initial Group roster persistence failed");

  GroupRosterSnapshot handoff = current;
  std::strcpy(handoff.leader_node_id, "namo-node-01");
  handoff.assignment_revision++;
  backend.fail_commit_write = true;
  require(store.update(backend, handoff, nullptr) ==
              GroupRosterStoreResult::STORAGE_FAILED,
          "injected Group roster commit failure was not reported");
  backend.fail_commit_write = false;

  GroupRosterStore failed_reboot_store;
  GroupRosterSnapshot failed_reboot{};
  require(failed_reboot_store.load(backend, &failed_reboot) ==
              GroupRosterStoreResult::LOADED &&
              std::strcmp(failed_reboot.leader_node_id, "namo-node-00") == 0,
          "failed Group handoff became visible after reboot");

  require(failed_reboot_store.update(backend, handoff, &handoff) ==
              GroupRosterStoreResult::UPDATED,
          "Group handoff retry did not persist");
  GroupRosterStore successful_reboot_store;
  GroupRosterSnapshot successful_reboot{};
  require(successful_reboot_store.load(backend, &successful_reboot) ==
              GroupRosterStoreResult::LOADED &&
              std::strcmp(successful_reboot.leader_node_id,
                          "namo-node-01") == 0 &&
              successful_reboot.member_count() == 7,
          "successful Group handoff did not survive reboot");
}

void stale_scope_is_rejected_but_reenrollment_can_replace_it() {
  auto current = populated_roster();
  current.assignment_revision = 40;

  auto stale = current;
  stale.assignment_revision = 39;
  std::strcpy(stale.leader_node_id, "namo-node-01");
  require(!group_roster_update_allowed(current, stale),
          "an older assignment replaced the same Group roster");

  auto reenrolled = current;
  std::strcpy(reenrolled.site_id, "site-new");
  std::strcpy(reenrolled.group_id, "group-new");
  std::strcpy(reenrolled.space_id, "room-new");
  reenrolled.assignment_revision = 1;
  for (auto &member : reenrolled.members) {
    if (!member.occupied)
      continue;
    std::strcpy(member.spatial_profile.group_id, "group-new");
    std::strcpy(member.spatial_profile.space_id, "room-new");
  }
  require(valid_group_roster(reenrolled) &&
              group_roster_update_allowed(current, reenrolled),
          "a valid re-enrollment was blocked by the previous Group revision");
}

void every_one_to_seven_member_transition_rebuilds_the_roster() {
  constexpr uint8_t ALL_MEMBERS = (1U << 7U) - 1U;
  for (uint8_t previous_mask = 1; previous_mask <= ALL_MEMBERS;
       ++previous_mask) {
    const auto previous_configuration =
        configuration_for_mask(previous_mask, 10);
    const auto *previous_leader = select_group_roster_leader(
        previous_configuration, "group-main", "namo-node-06", nullptr,
        nullptr, nullptr);
    require(previous_leader != nullptr,
            "a non-empty previous Group did not elect a leader");
    GroupRosterSnapshot previous{};
    require(prepare_group_roster_from_site_configuration(
                previous_configuration, "group-main",
                previous_leader->participant_id, &previous) ==
                GroupRosterResult::OK,
            "a previous Group mask did not produce a roster");

    for (uint8_t next_mask = 1; next_mask <= ALL_MEMBERS; ++next_mask) {
      const auto next_configuration = configuration_for_mask(next_mask, 11);
      const auto *selected = select_group_roster_leader(
          next_configuration, "group-main", "namo-node-06",
          previous.leader_node_id, previous.leader_node_id,
          previous.leader_node_id);
      require(selected != nullptr,
              "an add, leave, or rejoin transition lost Group authority");
      const auto *preserved = next_configuration.find(
          previous.leader_node_id);
      const bool preserved_allowed =
          preserved != nullptr &&
          group_leader_assignment_allowed(
              next_configuration, "group-main", "namo-node-06",
              previous.leader_node_id);
      if (preserved_allowed)
        require(std::strcmp(selected->participant_id,
                            previous.leader_node_id) == 0,
                "a valid Group leader changed during a roster transition");
      else {
        const auto *fallback = select_group_roster_leader(
            next_configuration, "group-main", "namo-node-06", nullptr,
            nullptr, nullptr);
        require(fallback != nullptr &&
                    std::strcmp(selected->participant_id,
                                fallback->participant_id) == 0,
                "leader departure did not select the deterministic fallback");
      }

      GroupRosterSnapshot candidate{};
      require(prepare_group_roster_from_site_configuration(
                  next_configuration, "group-main", selected->participant_id,
                  &candidate) == GroupRosterResult::OK &&
                  candidate.member_count() == mask_member_count(next_mask) &&
                  group_roster_update_allowed(previous, candidate),
              "a valid one-to-seven member transition was rejected");
      for (uint8_t index = 0; index < 7; ++index) {
        char participant_id[32]{};
        std::snprintf(participant_id, sizeof(participant_id),
                      "namo-node-%02u", static_cast<unsigned>(index));
        require((candidate.find(participant_id) != nullptr) ==
                    ((next_mask & (1U << index)) != 0),
                "a transition retained a departed member or lost a joiner");
      }
    }
  }
}

void site_and_group_leader_are_separated_for_two_to_seven_members() {
  constexpr uint8_t ALL_MEMBERS = (1U << 7U) - 1U;
  for (uint8_t mask = 1; mask <= ALL_MEMBERS; ++mask) {
    const auto configuration = configuration_for_mask(mask, 50);
    for (uint8_t site_leader_index = 0; site_leader_index < 7;
         ++site_leader_index) {
      if ((mask & (1U << site_leader_index)) == 0)
        continue;
      char site_leader_id[32]{};
      std::snprintf(site_leader_id, sizeof(site_leader_id),
                    "namo-node-%02u",
                    static_cast<unsigned>(site_leader_index));
      const auto *selected = select_group_roster_leader(
          configuration, "group-main", site_leader_id, site_leader_id,
          site_leader_id, site_leader_id);
      require(selected != nullptr,
              "role separation lost all Group leader candidates");
      if (mask_member_count(mask) == 1) {
        require(std::strcmp(selected->participant_id, site_leader_id) == 0,
                "a lone surviving Node could not retain both roles");
      } else {
        require(std::strcmp(selected->participant_id, site_leader_id) != 0,
                "a two-to-seven member Group retained the Site leader");
      }
    }
  }

  auto configuration = configuration_for_mask(0x7F, 51);
  std::strcpy(configuration.configuration_owner_node_id, "namo-node-00");
  const auto *owner_group_leader = select_group_roster_leader(
      configuration, "group-main", "namo-node-06", "namo-node-00",
      "namo-node-00", "namo-node-00");
  require(owner_group_leader != nullptr &&
              std::strcmp(owner_group_leader->participant_id,
                          "namo-node-00") == 0,
          "Configuration Owner plus one leader role was incorrectly blocked");
}

void triple_role_is_used_only_without_an_eligible_group_candidate() {
  auto split_groups = configuration_with_group(2);
  std::strcpy(split_groups.members[1].spatial_profile.space_id,
              "room-other");
  std::strcpy(split_groups.members[1].spatial_profile.group_id,
              "group-other");
  const auto *split_group_leader = select_group_roster_leader(
      split_groups, "group-main", "namo-node-00", "namo-node-00",
      "namo-node-00", "namo-node-00");
  require(split_group_leader != nullptr &&
              std::strcmp(split_group_leader->participant_id,
                          "namo-node-00") == 0,
          "an unrelated active Node blocked the only eligible Group leader");

  auto ineligible_peer = configuration_with_group(2);
  ineligible_peer.members[1].leader_eligible = false;
  const auto *emergency_leader = select_group_roster_leader(
      ineligible_peer, "group-main", "namo-node-00", "namo-node-00",
      "namo-node-00", "namo-node-00");
  require(emergency_leader != nullptr &&
              std::strcmp(emergency_leader->participant_id,
                          "namo-node-00") == 0,
          "a non-eligible peer blocked emergency triple-role operation");

  ineligible_peer.members[1].leader_eligible = true;
  const auto *replacement_leader = select_group_roster_leader(
      ineligible_peer, "group-main", "namo-node-00", "namo-node-00",
      "namo-node-00", "namo-node-00");
  require(replacement_leader != nullptr &&
              std::strcmp(replacement_leader->participant_id,
                          "namo-node-01") == 0,
          "an emergency triple-role leader did not yield to an eligible peer");

  const auto standalone = configuration_with_group(1);
  const auto *standalone_leader = select_group_roster_leader(
      standalone, "group-main", "namo-node-00", "namo-node-00",
      "namo-node-00", "namo-node-00");
  require(standalone_leader != nullptr &&
              std::strcmp(standalone_leader->participant_id,
                          "namo-node-00") == 0,
          "a standalone Node was prevented from holding all local roles");
}

void persisted_add_leave_and_rejoin_sequence_survives_every_reboot() {
  FakeBackend backend;
  GroupRosterStore store;
  GroupRosterSnapshot current{};
  uint32_t revision = 20;
  const std::array<uint8_t, 15> sequence{
      0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0x3F,
      0x1F, 0x0F, 0x07, 0x03, 0x01, 0x41, 0x7F};
  for (std::size_t step = 0; step < sequence.size(); ++step) {
    const auto configuration =
        configuration_for_mask(sequence[step], revision++);
    const auto *leader = select_group_roster_leader(
        configuration, "group-main", "namo-node-06", nullptr, nullptr,
        valid_group_roster(current) ? current.leader_node_id : nullptr);
    require(leader != nullptr,
            "persisted membership sequence lost its leader");
    GroupRosterSnapshot candidate{};
    require(prepare_group_roster_from_site_configuration(
                configuration, "group-main", leader->participant_id,
                &candidate) == GroupRosterResult::OK,
            "persisted membership sequence could not build a roster");
    const auto result = step == 0
                            ? store.create(backend, candidate, &current)
                            : store.update(backend, candidate, &current);
    require(result == (step == 0 ? GroupRosterStoreResult::CREATED
                                 : GroupRosterStoreResult::UPDATED),
            "persisted membership transition failed");

    GroupRosterStore rebooted_store;
    GroupRosterSnapshot rebooted{};
    require(rebooted_store.load(backend, &rebooted) ==
                GroupRosterStoreResult::LOADED &&
                rebooted.member_count() == mask_member_count(sequence[step]) &&
                same_group_roster_payload(current, rebooted),
            "membership transition did not survive reboot");
    store = GroupRosterStore{};
    current = rebooted;
  }
}

void stale_rosters_are_never_admitted_to_the_runtime() {
  const auto old_configuration = configuration_for_mask(0x01, 30);
  const auto *leader = select_group_roster_leader(
      old_configuration, "group-main", nullptr, nullptr, nullptr, nullptr);
  GroupRosterSnapshot roster{};
  require(leader != nullptr &&
              prepare_group_roster_from_site_configuration(
                  old_configuration, "group-main", leader->participant_id,
                  &roster) == GroupRosterResult::OK &&
              group_roster_current_for_local_node(
                  roster, old_configuration, "namo-node-00"),
          "current Group roster was not admitted");

  const auto expanded = configuration_for_mask(0x07, 31);
  require(!group_roster_current_for_local_node(
              roster, expanded, "namo-node-00"),
          "a one-member roster remained active after two members joined");
  auto departed = configuration_for_mask(0x06, 32);
  require(!group_roster_current_for_local_node(
              roster, departed, "namo-node-00"),
          "a departed local Node retained its stale Group runtime");

  auto moved_site = old_configuration;
  std::strcpy(moved_site.site_id, "site-reenrolled");
  require(!group_roster_matches_configuration_scope(
              roster, moved_site, "group-main") &&
              !group_roster_current_for_local_node(
                  roster, moved_site, "namo-node-00"),
          "a Group roster from the previous Site remained a leader candidate");
}

void revoked_moved_and_ineligible_members_follow_canonical_policy() {
  auto configuration = configuration_for_mask(0x7F, 40);
  configuration.members[0].membership = ParticipantMembership::REVOKED;
  configuration.members[1].membership = ParticipantMembership::QUARANTINED;
  std::strcpy(configuration.members[2].spatial_profile.group_id,
              "group-other");
  std::strcpy(configuration.members[2].spatial_profile.space_id,
              "room-other");
  configuration.members[3].leader_eligible = false;
  const auto *leader = select_group_roster_leader(
      configuration, "group-main", "namo-node-04", "namo-node-00",
      "namo-node-01", "namo-node-02");
  require(leader != nullptr &&
              std::strcmp(leader->participant_id, "namo-node-05") == 0,
          "invalid prior leaders blocked deterministic replacement");
  GroupRosterSnapshot roster{};
  require(prepare_group_roster_from_site_configuration(
              configuration, "group-main", leader->participant_id,
              &roster) == GroupRosterResult::OK &&
              roster.member_count() == 4 &&
              roster.find("namo-node-00") == nullptr &&
              roster.find("namo-node-01") == nullptr &&
              roster.find("namo-node-02") == nullptr &&
              roster.find("namo-node-03") != nullptr,
          "canonical membership policy did not prune invalid members");
}

void eight_groups_keep_fifty_six_members_out_of_site_directory() {
  std::array<GroupRosterSnapshot, 8> rosters{};
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-home");
  directory.generation = 1;
  directory.topology_revision = 1;
  directory.committed = true;
  auto &floorplan = directory.floorplans[0];
  floorplan.occupied = true;
  floorplan.committed = true;
  std::strcpy(floorplan.floorplan_id, "floorplan-main");
  std::strcpy(floorplan.owner_node_id, "node-0-0");
  floorplan.owner_generation = 1;
  floorplan.geometry_version = 5;
  floorplan.config_hash = 101;
  floorplan.image_hash = 202;
  for (std::size_t group_index = 0; group_index < rosters.size();
       ++group_index) {
    auto configuration = configuration_with_group();
    char group_id[24]{};
    char space_id[24]{};
    char leader_id[24]{};
    std::snprintf(group_id, sizeof(group_id), "group-%u",
                  static_cast<unsigned>(group_index));
    std::snprintf(space_id, sizeof(space_id), "room-%u",
                  static_cast<unsigned>(group_index));
    std::snprintf(leader_id, sizeof(leader_id), "node-%u-0",
                  static_cast<unsigned>(group_index));
    std::strcpy(configuration.configuration_owner_node_id, leader_id);
    for (std::size_t member_index = 0; member_index < 7; ++member_index) {
      auto &member = configuration.members[member_index];
      std::snprintf(member.participant_id, sizeof(member.participant_id),
                    "node-%u-%u", static_cast<unsigned>(group_index),
                    static_cast<unsigned>(member_index));
      std::strcpy(member.spatial_profile.group_id, group_id);
      std::strcpy(member.spatial_profile.space_id, space_id);
    }
    require(prepare_group_roster_from_site_configuration(
                configuration, group_id, leader_id,
                &rosters[group_index]) == GroupRosterResult::OK,
            "one of eight independent Group rosters was rejected");

    auto &entry = directory.groups[group_index];
    entry.occupied = true;
    std::strcpy(entry.group_id, group_id);
    std::strcpy(entry.floorplan_id, "floorplan-main");
    std::strcpy(entry.leader_node_id, leader_id);
    entry.assignment_revision = rosters[group_index].assignment_revision;
    entry.member_count = static_cast<uint8_t>(rosters[group_index].member_count());
  }
  std::size_t aggregate_members = 0;
  for (const auto &roster : rosters)
    aggregate_members += roster.member_count();
  require(aggregate_members == 56 && directory.group_count() == 8 &&
              valid_site_directory(directory) &&
              sizeof(directory.groups) ==
                  sizeof(GroupDirectoryEntry) * 8,
          "the Site directory retained more than eight bounded summaries");
}

}  // namespace

void run_group_roster_tests() {
  seven_member_roster_is_bounded_and_round_trips();
  legacy_configuration_migrates_once();
  empty_migration_retries_when_configuration_becomes_ready();
  leader_handoff_is_atomic_across_reboot();
  stale_scope_is_rejected_but_reenrollment_can_replace_it();
  every_one_to_seven_member_transition_rebuilds_the_roster();
  site_and_group_leader_are_separated_for_two_to_seven_members();
  triple_role_is_used_only_without_an_eligible_group_candidate();
  persisted_add_leave_and_rejoin_sequence_survives_every_reboot();
  stale_rosters_are_never_admitted_to_the_runtime();
  revoked_moved_and_ineligible_members_follow_canonical_policy();
  eight_groups_keep_fifty_six_members_out_of_site_directory();
}
