#include "site_directory.h"
#include "platform_runtime_workspace.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "site_directory_tests: %s\n", message);
  std::exit(1);
}

class FakeBackend : public SiteDirectoryStorageBackend {
 public:
  FakeBackend() {
    for (auto &slot : this->slots)
      slot.bytes.fill(0xFF);
    this->commit.bytes.fill(0xFF);
  }

  bool load_site_directory_slot(
      uint8_t slot, SiteDirectoryStorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr || this->fail_load)
      return false;
    *blob = this->slots[slot];
    return true;
  }

  bool save_site_directory_slot(
      uint8_t slot, const SiteDirectoryStorageBlob &blob) override {
    if (slot >= 2 || this->fail_slot_write)
      return false;
    this->slots[slot] = blob;
    return true;
  }

  bool load_site_directory_commit(
      SiteDirectoryCommitBlob *blob) override {
    if (blob == nullptr || this->fail_load)
      return false;
    *blob = this->commit;
    return true;
  }

  bool save_site_directory_commit(
      const SiteDirectoryCommitBlob &blob) override {
    if (this->fail_commit_write)
      return false;
    this->commit = blob;
    return true;
  }

  bool sync() override {
    this->sync_count++;
    return !this->fail_sync;
  }

  SiteDirectoryStorageBlob slots[2]{};
  SiteDirectoryCommitBlob commit{};
  bool fail_load{false};
  bool fail_slot_write{false};
  bool fail_commit_write{false};
  bool fail_sync{false};
  uint32_t sync_count{0};
};

SiteDirectorySnapshot populated_directory(std::size_t count = 8) {
  SiteDirectorySnapshot directory{};
  std::strcpy(directory.site_id, "site-home");
  directory.generation = 7;
  directory.topology_revision = 11;
  directory.committed = true;
  for (std::size_t index = 0; index < count; ++index) {
    auto &floorplan = directory.floorplans[index];
    floorplan.occupied = true;
    floorplan.committed = true;
    std::snprintf(floorplan.floorplan_id, sizeof(floorplan.floorplan_id),
                  "floorplan-%u", static_cast<unsigned>(index));
    std::snprintf(floorplan.owner_node_id,
                  sizeof(floorplan.owner_node_id), "namo-owner-%u",
                  static_cast<unsigned>(index));
    floorplan.owner_generation = static_cast<uint32_t>(index + 1);
    floorplan.geometry_version = static_cast<uint32_t>(index + 3);
    floorplan.config_hash = static_cast<uint32_t>(100 + index);
    floorplan.image_hash = static_cast<uint32_t>(200 + index);

    auto &group = directory.groups[index];
    group.occupied = true;
    std::snprintf(group.group_id, sizeof(group.group_id), "group-%u",
                  static_cast<unsigned>(index));
    std::strcpy(group.floorplan_id, floorplan.floorplan_id);
    std::snprintf(group.leader_node_id, sizeof(group.leader_node_id),
                  "namo-leader-%u", static_cast<unsigned>(index));
    group.assignment_revision = static_cast<uint32_t>(index + 1);
    group.member_count =
        static_cast<uint8_t>(SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS);
  }
  return directory;
}

ParticipantReadinessConfig projection_readiness(const char *group_id) {
  ParticipantReadinessConfig config{};
  config.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                            PARTICIPANT_CAP_VELOCITY |
                            PARTICIPANT_CAP_RANGE |
                            PARTICIPANT_CAP_PRESENCE |
                            PARTICIPANT_CAP_MOTION;
  config.max_targets = 3;
  std::strcpy(config.floorplan_id, "floorplan-main");
  std::strcpy(config.space_id, "room-main");
  std::strcpy(config.group_id, group_id);
  config.geometry_version = 4;
  config.local_pose_version = 1;
  config.pose_version = 1;
  config.calibration_version = 1;
  config.transform.valid = true;
  config.transform.sample_count = 1;
  return config;
}

SiteSpatialProfile projection_profile(const char *group_id) {
  SiteSpatialProfile profile{};
  profile.committed = true;
  std::strcpy(profile.space_id, "room-main");
  std::strcpy(profile.group_id, group_id);
  profile.pose_version = 1;
  profile.calibration_version = 1;
  profile.transform.valid = true;
  profile.transform.sample_count = 1;
  return profile;
}

SiteConfigurationSnapshot projection_configuration(
    PlatformIdentity *local_identity, ParticipantRegistry *participants,
    ParticipantReadinessRegistry *readiness) {
  std::strcpy(local_identity->site_id, "site-home");
  std::strcpy(local_identity->node_id, "namo-node-z");
  require(participants->discover("namo-node-a", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK &&
              participants->classify(
                  "namo-node-a", ParticipantTrust::OFFICIAL_VERIFIED) ==
                  ParticipantRegistryResult::OK &&
              readiness->configure("namo-node-a",
                                   projection_readiness("group-main")) ==
                  ParticipantReadinessResult::OK &&
              participants->activate("namo-node-a") ==
                  ParticipantRegistryResult::OK,
          "projection participant fixture failed");

  SiteConfigurationSnapshot initial{};
  require(initialize_site_configuration(*local_identity, false, &initial) ==
              SiteConfigurationResult::OK,
          "projection configuration bootstrap failed");
  SiteConfigurationSnapshot roster{};
  require(commit_local_site_roster(*local_identity, *participants, initial,
                                   &roster) ==
              SiteConfigurationResult::OK,
          "projection roster commit failed");
  SiteConfigurationSnapshot configuration{};
  require(commit_local_floorplan_manifest(
              *local_identity, roster, "floorplan-main", 4, 101, 202,
              &configuration) == SiteConfigurationResult::OK,
          "projection Floorplan commit failed");
  for (auto &member : configuration.members) {
    if (member.occupied)
      member.spatial_profile = projection_profile("group-main");
  }
  return configuration;
}

void fixed_limits_and_role_boundaries_are_enforced() {
  auto directory = populated_directory();
  require(SiteDirectoryLimits::MAX_FLOORPLANS == 8 &&
              SiteDirectoryLimits::MAX_GROUPS == 8 &&
              SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS == 7 &&
              SiteDirectoryLimits::MAX_SITE_GROUP_PARTICIPANT_REFERENCES ==
                  56,
          "hierarchy limits changed unexpectedly");
  require(valid_site_directory(directory) &&
              directory.floorplan_count() == 8 &&
              directory.group_count() == 8,
          "maximum bounded directory should be valid");

  auto invalid = directory;
  invalid.groups[0].member_count = 8;
  require(!valid_site_directory(invalid),
          "a Group must reject an eighth sensing participant");
  invalid = directory;
  std::strcpy(invalid.groups[1].leader_node_id,
              invalid.groups[0].leader_node_id);
  require(!valid_site_directory(invalid),
          "one Node must not lead two Groups");
  invalid = directory;
  std::strcpy(invalid.floorplans[1].owner_node_id,
              invalid.floorplans[0].owner_node_id);
  require(!valid_site_directory(invalid),
          "one Node must not own two active Floorplans");
  invalid = directory;
  invalid.compatibility_mirror = true;
  invalid.legacy_single_floorplan_projection = true;
  require(!valid_site_directory(invalid),
          "a directory must not use both migration provenance flags");
}

void codec_round_trip_is_bounded_and_corruption_fails_closed() {
  auto source = populated_directory();
  std::array<uint8_t, SiteDirectoryCodec::MAX_ENCODED_SIZE> bytes{};
  std::size_t encoded_size = 0;
  require(SiteDirectoryCodec::encode(
              source, bytes.data(), bytes.size(), &encoded_size) ==
              SiteDirectoryCodecResult::OK &&
              encoded_size < SiteDirectoryCodec::MAX_ENCODED_SIZE,
          "maximum directory did not fit its storage budget");

  SiteDirectorySnapshot decoded{};
  require(SiteDirectoryCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteDirectoryCodecResult::OK &&
              decoded.floorplan_count() == 8 &&
              decoded.group_count() == 8 &&
              !decoded.legacy_single_floorplan_projection &&
              std::strcmp(decoded.groups[7].leader_node_id,
                          "namo-leader-7") == 0,
          "directory codec round trip failed");

  bytes[encoded_size - 1] ^= 0x80;
  require(SiteDirectoryCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteDirectoryCodecResult::CORRUPT,
          "directory checksum corruption must fail closed");
  require(SiteDirectoryCodec::encode(
              source, bytes.data(), bytes.size(), &encoded_size) ==
              SiteDirectoryCodecResult::OK,
          "directory re-encode failed");
  bytes[encoded_size + 10] = 0;
  require(SiteDirectoryCodec::decode(bytes.data(), bytes.size(), &decoded) ==
              SiteDirectoryCodecResult::CORRUPT,
          "directory trailing flash corruption must fail closed");

  auto projection = populated_directory(1);
  projection.legacy_single_floorplan_projection = true;
  require(SiteDirectoryCodec::encode(
              projection, bytes.data(), bytes.size(), &encoded_size) ==
              SiteDirectoryCodecResult::OK &&
              SiteDirectoryCodec::decode(bytes.data(), bytes.size(),
                                         &decoded) ==
                  SiteDirectoryCodecResult::OK &&
              decoded.legacy_single_floorplan_projection,
          "legacy projection provenance did not survive codec round trip");
}

void single_floorplan_configuration_migrates_without_guessing_groups() {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-home");
  std::strcpy(identity.node_id, "namo-node-owner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(identity, false, &configuration) ==
              SiteConfigurationResult::OK,
          "legacy configuration bootstrap failed");
  SiteConfigurationSnapshot committed{};
  require(commit_local_floorplan_manifest(
              identity, configuration, "floorplan-main", 4, 101, 202,
              &committed) == SiteConfigurationResult::OK,
          "legacy floorplan commit failed");

  SiteDirectorySnapshot directory{};
  require(migrate_site_configuration_to_directory(committed, &directory) ==
              SiteDirectoryResult::OK &&
              directory.floorplan_count() == 1 &&
              directory.group_count() == 0 &&
              directory.compatibility_mirror &&
              std::strcmp(directory.floorplans[0].floorplan_id,
                          "floorplan-main") == 0 &&
              std::strcmp(directory.floorplans[0].owner_node_id,
                          identity.node_id) == 0 &&
              directory.floorplans[0].geometry_version == 4,
          "single Floorplan migration lost authoritative metadata");
}

void compatibility_mirror_tracks_legacy_boots_without_overwriting_canonical_state() {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-home");
  std::strcpy(identity.node_id, "namo-node-owner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(identity, false, &configuration) ==
              SiteConfigurationResult::OK,
          "compatibility mirror bootstrap failed");

  SiteDirectorySnapshot candidate{};
  bool update_required = false;
  require(prepare_site_directory_compatibility_mirror(
              configuration, nullptr, &candidate, &update_required) ==
              SiteDirectoryResult::OK &&
              update_required && candidate.compatibility_mirror,
          "an empty directory must create a compatibility mirror");

  SiteDirectorySnapshot unchanged{};
  require(prepare_site_directory_compatibility_mirror(
              configuration, &candidate, &unchanged, &update_required) ==
              SiteDirectoryResult::OK &&
              !update_required &&
              unchanged.generation == candidate.generation,
          "an unchanged legacy configuration must not wear flash");

  auto in_place = candidate;
  require(prepare_site_directory_compatibility_mirror(
              configuration, &in_place, &in_place, &update_required) ==
              SiteDirectoryResult::OK &&
              !update_required && in_place.compatibility_mirror &&
              in_place.generation == candidate.generation,
          "an in-place boot reconciliation must preserve the mirror");

  SiteConfigurationSnapshot committed{};
  require(commit_local_floorplan_manifest(
              identity, configuration, "floorplan-main", 2, 101, 202,
              &committed) == SiteConfigurationResult::OK,
          "compatibility mirror floorplan commit failed");
  SiteDirectorySnapshot updated{};
  require(prepare_site_directory_compatibility_mirror(
              committed, &candidate, &updated, &update_required) ==
              SiteDirectoryResult::OK &&
              update_required && updated.floorplan_count() == 1,
          "a changed legacy configuration must update its mirror");

  auto canonical = populated_directory(2);
  canonical.compatibility_mirror = false;
  SiteDirectorySnapshot preserved{};
  require(prepare_site_directory_compatibility_mirror(
              committed, &canonical, &preserved, &update_required) ==
              SiteDirectoryResult::OK &&
              !update_required && preserved.floorplan_count() == 2 &&
              !preserved.compatibility_mirror,
          "legacy boot state must not overwrite a canonical directory");
}

void foreign_canonical_directory_is_rebased_to_the_current_site() {
  PlatformIdentity identity{};
  std::strcpy(identity.site_id, "site-current");
  std::strcpy(identity.node_id, "namo-node-current");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(identity, false, &configuration) ==
              SiteConfigurationResult::OK,
          "current Site configuration bootstrap failed");

  auto foreign = populated_directory(2);
  std::strcpy(foreign.site_id, "site-previous");
  foreign.compatibility_mirror = false;
  require(!site_directory_matches_configuration_scope(configuration, foreign),
          "a canonical Directory from another Site matched the current scope");

  SiteDirectorySnapshot rebased{};
  bool update_required = false;
  require(prepare_site_directory_compatibility_mirror(
              configuration, &foreign, &rebased, &update_required) ==
                  SiteDirectoryResult::OK &&
              update_required && rebased.compatibility_mirror &&
              std::strcmp(rebased.site_id, configuration.site_id) == 0 &&
              rebased.floorplan_count() == 0 && rebased.group_count() == 0 &&
              site_directory_matches_configuration_scope(configuration,
                                                         rebased),
          "a foreign canonical Directory survived the Site scope transition");

  FakeBackend backend{};
  SiteDirectoryStore store{};
  SiteDirectorySnapshot persisted{};
  require(store.create(backend, foreign, &persisted) ==
              SiteDirectoryStoreResult::CREATED,
          "foreign Directory persistence fixture failed");
  backend.fail_commit_write = true;
  require(store.update(backend, rebased, &persisted) ==
              SiteDirectoryStoreResult::STORAGE_FAILED,
          "a failed Site scope rebase was not reported");
  SiteDirectoryStore failed_reboot_store{};
  SiteDirectorySnapshot failed_reboot{};
  require(failed_reboot_store.load(backend, &failed_reboot) ==
                  SiteDirectoryStoreResult::LOADED &&
              std::strcmp(failed_reboot.site_id, "site-previous") == 0,
          "a failed Site scope rebase published an uncommitted Directory");

  backend.fail_commit_write = false;
  require(prepare_site_directory_compatibility_mirror(
              configuration, &failed_reboot, &rebased, &update_required) ==
                  SiteDirectoryResult::OK &&
              update_required &&
              failed_reboot_store.update(backend, rebased, &failed_reboot) ==
                  SiteDirectoryStoreResult::UPDATED,
          "Site scope rebase did not recover on retry");
  SiteDirectoryStore successful_reboot_store{};
  SiteDirectorySnapshot successful_reboot{};
  require(successful_reboot_store.load(backend, &successful_reboot) ==
                  SiteDirectoryStoreResult::LOADED &&
              site_directory_matches_configuration_scope(configuration,
                                                         successful_reboot),
          "the rebased Directory did not survive reboot");
}

void committed_single_floorplan_state_promotes_without_guessing() {
  PlatformIdentity local{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  auto configuration =
      projection_configuration(&local, &participants, &readiness);
  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  GroupHeartbeat remote{};
  remote.participant_id = "namo-node-a";
  remote.group_id = "group-main";
  remote.boot_id = "boot-a";
  remote.sequence = 1;
  remote.term = 1;
  remote.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote, 1000) ==
              GroupHeartbeatResult::OK,
          "projection Group peer heartbeat was not admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);

  SiteDirectorySnapshot mirror{};
  require(migrate_site_configuration_to_directory(configuration, &mirror) ==
              SiteDirectoryResult::OK,
          "projection mirror migration failed");
  SiteDirectorySnapshot projection{};
  bool update_required = false;
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, mirror, &projection,
              &update_required) == SiteDirectoryResult::OK &&
              update_required && !projection.compatibility_mirror &&
              projection.legacy_single_floorplan_projection &&
              projection.floorplan_count() == 1 &&
              projection.group_count() == 1 &&
              projection.groups[0].member_count == 2 &&
              std::strcmp(projection.groups[0].group_id,
                          "group-main") == 0 &&
              std::strcmp(projection.groups[0].leader_node_id,
                          local.node_id) == 0,
          "committed Group state did not promote deterministically");

  projection.groups[0].assignment_revision = 9;
  SiteDirectorySnapshot unchanged{};
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, projection, &unchanged,
              &update_required) == SiteDirectoryResult::OK &&
              !update_required &&
              unchanged.legacy_single_floorplan_projection &&
              unchanged.groups[0].assignment_revision == 9,
          "a runtime Group term changed a persistent assignment revision");

  auto future_canonical = populated_directory(2);
  SiteDirectorySnapshot preserved{};
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, future_canonical, &preserved,
              &update_required) == SiteDirectoryResult::OK &&
              !update_required && preserved.floorplan_count() == 2 &&
              !preserved.legacy_single_floorplan_projection,
          "legacy projection must not overwrite future canonical state");
}

void incomplete_or_changed_topology_fails_closed() {
  PlatformIdentity local{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  auto configuration =
      projection_configuration(&local, &participants, &readiness);
  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  GroupHeartbeat remote{};
  remote.participant_id = "namo-node-a";
  remote.group_id = "group-main";
  remote.boot_id = "boot-a";
  remote.sequence = 1;
  remote.term = 1;
  remote.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote, 1000) ==
              GroupHeartbeatResult::OK,
          "deletion projection Group peer heartbeat was not admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  SiteDirectorySnapshot mirror{};
  require(migrate_site_configuration_to_directory(configuration, &mirror) ==
              SiteDirectoryResult::OK,
          "incomplete projection mirror migration failed");

  auto incomplete = configuration;
  for (auto &member : incomplete.members) {
    if (member.occupied &&
        std::strcmp(member.participant_id, "namo-node-a") == 0) {
      member.spatial_profile = {};
      break;
    }
  }
  SiteDirectorySnapshot candidate{};
  bool update_required = false;
  require(prepare_legacy_single_floorplan_projection(
              incomplete, coordinator, mirror, &candidate,
              &update_required) == SiteDirectoryResult::NOT_READY,
          "an active member without a committed Group must block promotion");

  auto too_many = configuration;
  for (std::size_t index = 2; index < 8; ++index) {
    auto &member = too_many.members[index];
    member.occupied = true;
    std::snprintf(member.participant_id, sizeof(member.participant_id),
                  "namo-node-extra-%u", static_cast<unsigned>(index));
    member.type = ParticipantType::NAMO_NODE;
    member.trust = ParticipantTrust::OFFICIAL_VERIFIED;
    member.membership = ParticipantMembership::ACTIVE;
    member.leader_eligible = true;
    member.spatial_profile = projection_profile("group-main");
  }
  require(prepare_legacy_single_floorplan_projection(
              too_many, coordinator, mirror, &candidate,
              &update_required) == SiteDirectoryResult::FULL,
          "an eighth sensing input must be rejected before persistence");

  SiteDirectorySnapshot projection{};
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, mirror, &projection,
              &update_required) == SiteDirectoryResult::OK &&
              update_required,
          "deletion fixture projection failed");
  auto deleted = configuration;
  deleted.floorplan = {};
  require(prepare_legacy_single_floorplan_projection(
              deleted, coordinator, projection, &candidate,
              &update_required) == SiteDirectoryResult::OK &&
              update_required && candidate.compatibility_mirror &&
              !candidate.legacy_single_floorplan_projection &&
              candidate.floorplan_count() == 0 &&
              candidate.group_count() == 0,
          "Floorplan deletion must remove the legacy projection");
}

void store_keeps_the_last_commit_and_repairs_a_damaged_active_slot() {
  FakeBackend backend{};
  SiteDirectoryStore store{};
  SiteDirectorySnapshot stored{};
  auto initial = populated_directory(1);
  require(store.create(backend, initial, &stored) ==
              SiteDirectoryStoreResult::CREATED &&
              stored.generation == 1,
          "directory store create failed");

  auto updated = stored;
  updated.topology_revision++;
  require(store.update(backend, updated, &stored) ==
              SiteDirectoryStoreResult::UPDATED &&
              stored.generation == 2,
          "directory store update failed");

  backend.slots[1].bytes[40] ^= 0x01;
  SiteDirectorySnapshot recovered{};
  require(store.load(backend, &recovered) ==
              SiteDirectoryStoreResult::COMMIT_REPAIRED &&
              recovered.generation == 1,
          "a damaged active slot must fall back to the valid committed copy");
  require(store.load(backend, &recovered) ==
              SiteDirectoryStoreResult::LOADED &&
              recovered.generation == 1,
          "the repaired commit marker did not survive reload");
}

void failed_commit_does_not_publish_the_new_slot() {
  FakeBackend backend{};
  SiteDirectoryStore store{};
  SiteDirectorySnapshot stored{};
  auto initial = populated_directory(1);
  require(store.create(backend, initial, &stored) ==
              SiteDirectoryStoreResult::CREATED,
          "power-loss fixture create failed");
  auto updated = stored;
  updated.topology_revision++;
  backend.fail_commit_write = true;
  require(store.update(backend, updated, &stored) ==
              SiteDirectoryStoreResult::STORAGE_FAILED,
          "commit write failure must be reported");
  backend.fail_commit_write = false;

  SiteDirectorySnapshot loaded{};
  require(store.load(backend, &loaded) ==
              SiteDirectoryStoreResult::LOADED &&
              loaded.generation == 1,
          "an uncommitted inactive slot must not replace the active directory");
}

void seven_member_handover_directory_is_atomic_across_reboot() {
  PlatformIdentity owner{};
  std::strcpy(owner.site_id, "site-home");
  std::strcpy(owner.node_id, "namo-node-owner");
  SiteConfigurationSnapshot configuration{};
  require(initialize_site_configuration(owner, false, &configuration) ==
              SiteConfigurationResult::OK,
          "seven-member Directory handover bootstrap failed");
  ParticipantRegistry participants{};
  const std::array<const char *, 6> peers{
      "namo-node-a", "namo-node-b", "namo-node-c",
      "namo-node-d", "namo-node-e", "namo-node-f"};
  for (const char *peer : peers) {
    require(participants.discover(peer, ParticipantType::NAMO_NODE) ==
                    ParticipantRegistryResult::OK &&
                participants.classify(
                    peer, ParticipantTrust::OFFICIAL_VERIFIED) ==
                    ParticipantRegistryResult::OK &&
                participants.activate(peer) == ParticipantRegistryResult::OK,
            "seven-member Directory participant setup failed");
  }
  require(commit_local_site_roster(owner, participants, configuration,
                                   &configuration) ==
                  SiteConfigurationResult::OK &&
              configuration.member_count() == 7,
          "seven-member Directory roster failed");
  SiteConfigurationSnapshot with_floorplan{};
  require(commit_local_floorplan_manifest(
              owner, configuration, "floorplan-main", 9, 0x10203040U,
              0x50607080U, &with_floorplan) == SiteConfigurationResult::OK,
          "seven-member Directory Floorplan failed");
  configuration = with_floorplan;
  for (auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    member.spatial_profile = projection_profile("group-main");
  }

  SiteDirectorySnapshot current{};
  require(migrate_site_configuration_to_directory(configuration, &current) ==
                  SiteDirectoryResult::OK &&
              current.floorplan_count() == 1 &&
              std::strcmp(current.floorplans[0].owner_node_id,
                          owner.node_id) == 0,
          "seven-member compatibility Directory migration failed");
  const auto *successor = automatic_configuration_successor(configuration);
  require(successor != nullptr,
          "seven-member Directory handover did not select a successor");
  PlatformIdentity successor_identity = owner;
  std::strcpy(successor_identity.node_id, successor->participant_id);
  SiteConfigurationSnapshot handed_over{};
  require(prepare_automatic_configuration_handover(
              successor_identity, configuration, &handed_over) ==
                  SiteConfigurationResult::OK,
          "seven-member Directory handover snapshot failed");
  SiteDirectorySnapshot candidate{};
  bool update_required = false;
  require(prepare_site_directory_compatibility_mirror(
              handed_over, &current, &candidate, &update_required) ==
                  SiteDirectoryResult::OK &&
              update_required && candidate.floorplan_count() == 1 &&
              std::strcmp(candidate.floorplans[0].owner_node_id,
                          successor_identity.node_id) == 0 &&
              candidate.floorplans[0].geometry_version == 9 &&
              candidate.floorplans[0].config_hash == 0x10203040U &&
              candidate.floorplans[0].image_hash == 0x50607080U,
          "seven-member handover did not rebuild the Floorplan authority metadata");

  FakeBackend backend{};
  SiteDirectoryStore store{};
  SiteDirectorySnapshot persisted{};
  require(store.create(backend, current, &persisted) ==
              SiteDirectoryStoreResult::CREATED,
          "seven-member Directory persistence bootstrap failed");
  candidate.generation = persisted.generation;
  backend.fail_commit_write = true;
  require(store.update(backend, candidate, &persisted) ==
              SiteDirectoryStoreResult::STORAGE_FAILED,
          "seven-member Directory commit failure was not reported");
  backend.fail_commit_write = false;
  SiteDirectoryStore failed_reboot_store{};
  SiteDirectorySnapshot failed_reboot{};
  require(failed_reboot_store.load(backend, &failed_reboot) ==
                  SiteDirectoryStoreResult::LOADED &&
              std::strcmp(failed_reboot.floorplans[0].owner_node_id,
                          owner.node_id) == 0 &&
              failed_reboot.floorplans[0].config_hash == 0x10203040U &&
              failed_reboot.floorplans[0].image_hash == 0x50607080U,
          "reboot after Directory handover failure did not restore the old authority");
  require(failed_reboot_store.update(backend, candidate, &failed_reboot) ==
              SiteDirectoryStoreResult::UPDATED,
          "seven-member Directory handover retry did not persist");
  SiteDirectoryStore successful_reboot_store{};
  SiteDirectorySnapshot successful_reboot{};
  require(successful_reboot_store.load(backend, &successful_reboot) ==
                  SiteDirectoryStoreResult::LOADED &&
              std::strcmp(successful_reboot.floorplans[0].owner_node_id,
                          successor_identity.node_id) == 0 &&
              successful_reboot.floorplans[0].geometry_version == 9 &&
              successful_reboot.floorplans[0].config_hash == 0x10203040U &&
              successful_reboot.floorplans[0].image_hash == 0x50607080U,
          "successful Directory handover did not survive reboot");
}

void authenticated_advertisements_promote_the_legacy_projection() {
  PlatformIdentity local{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  auto configuration =
      projection_configuration(&local, &participants, &readiness);
  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  GroupHeartbeat remote{};
  remote.participant_id = "namo-node-a";
  remote.group_id = "group-main";
  remote.boot_id = "boot-a";
  remote.sequence = 1;
  remote.term = 1;
  remote.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote, 1000) ==
              GroupHeartbeatResult::OK,
          "advertisement Group peer heartbeat was not admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  SiteDirectorySnapshot mirror{};
  require(migrate_site_configuration_to_directory(configuration, &mirror) ==
              SiteDirectoryResult::OK,
          "advertisement mirror migration failed");
  SiteDirectorySnapshot projection{};
  bool update_required = false;
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, mirror, &projection,
              &update_required) == SiteDirectoryResult::OK &&
              update_required,
          "advertisement projection failed");

  SiteFloorplanOwnerCache owners{};
  SiteFloorplanOwnerReport owner_report{};
  owner_report.participant_id = local.node_id;
  owner_report.floorplan_id = configuration.floorplan.floorplan_id;
  owner_report.owner_generation = configuration.generation;
  owner_report.geometry_version = configuration.floorplan.geometry_version;
  owner_report.config_hash = configuration.floorplan.config_hash;
  owner_report.image_hash = configuration.floorplan.image_hash;
  owner_report.lease_ms = 6000;
  require(owners.accept(configuration, owner_report, 1000) ==
              SiteFloorplanOwnerResult::OK,
          "local Floorplan owner advertisement was rejected");

  SiteGroupSummaryCache summaries{};
  SiteGroupSummaryReport group_report{};
  group_report.participant_id = local.node_id;
  group_report.group_id = projection.groups[0].group_id;
  group_report.assignment_revision =
      projection.groups[0].assignment_revision;
  group_report.term = projection.groups[0].assignment_revision;
  group_report.lease_ms = 6000;
  group_report.participants_expected = projection.groups[0].member_count;
  group_report.participants_online = projection.groups[0].member_count;
  require(summaries.accept(projection, group_report, 1000) ==
              SiteGroupSummaryResult::OK,
          "local Group summary was rejected");

  SiteDirectorySnapshot canonical{};
  require(prepare_site_directory_from_advertisements(
              configuration, projection, owners, summaries, &canonical,
              &update_required) == SiteDirectoryResult::OK &&
              update_required && canonical.committed &&
              !canonical.compatibility_mirror &&
              !canonical.legacy_single_floorplan_projection &&
              canonical.floorplan_count() == 1 &&
              canonical.group_count() == 1 &&
              canonical.topology_revision > projection.topology_revision,
          "authenticated metadata did not promote the canonical directory");
}

void committed_owner_handover_updates_the_canonical_directory() {
  PlatformIdentity local{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  auto configuration =
      projection_configuration(&local, &participants, &readiness);
  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  GroupHeartbeat remote{};
  remote.participant_id = "namo-node-a";
  remote.group_id = "group-main";
  remote.boot_id = "boot-a";
  remote.sequence = 1;
  remote.term = 1;
  remote.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote, 1000) ==
              GroupHeartbeatResult::OK,
          "handover Group peer heartbeat was not admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  SiteDirectorySnapshot mirror{};
  require(migrate_site_configuration_to_directory(configuration, &mirror) ==
              SiteDirectoryResult::OK,
          "handover directory migration failed");
  SiteDirectorySnapshot current{};
  bool update_required = false;
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, mirror, &current,
              &update_required) == SiteDirectoryResult::OK &&
              update_required,
          "handover projection failed");

  const auto *successor = automatic_configuration_successor(configuration);
  require(successor != nullptr,
          "handover fixture has no deterministic successor");
  std::strcpy(configuration.configuration_owner_node_id,
              successor->participant_id);
  ++configuration.generation;

  SiteFloorplanOwnerCache owners{};
  SiteFloorplanOwnerReport owner_report{};
  owner_report.participant_id = successor->participant_id;
  owner_report.floorplan_id = configuration.floorplan.floorplan_id;
  owner_report.owner_generation = configuration.generation;
  owner_report.geometry_version = configuration.floorplan.geometry_version;
  owner_report.config_hash = configuration.floorplan.config_hash;
  owner_report.image_hash = configuration.floorplan.image_hash;
  owner_report.lease_ms = 6000;
  require(owners.accept(configuration, owner_report, 1000) ==
              SiteFloorplanOwnerResult::OK,
          "committed successor advertisement was rejected");

  SiteGroupSummaryCache summaries{};
  SiteGroupSummaryReport group_report{};
  group_report.participant_id = current.groups[0].leader_node_id;
  group_report.group_id = current.groups[0].group_id;
  group_report.assignment_revision =
      current.groups[0].assignment_revision;
  group_report.term = current.groups[0].assignment_revision;
  group_report.lease_ms = 6000;
  group_report.participants_expected = current.groups[0].member_count;
  group_report.participants_online = current.groups[0].member_count;
  require(summaries.accept(current, group_report, 1000) ==
              SiteGroupSummaryResult::OK,
          "handover Group summary was rejected");

  SiteDirectorySnapshot candidate{};
  require(prepare_site_directory_from_advertisements(
              configuration, current, owners, summaries, &candidate,
              &update_required) == SiteDirectoryResult::OK &&
              update_required &&
              std::strcmp(candidate.floorplans[0].owner_node_id,
                          successor->participant_id) == 0 &&
              candidate.floorplans[0].owner_generation ==
                  configuration.generation,
          "committed owner handover did not update the canonical directory");

  std::strcpy(configuration.configuration_owner_node_id, local.node_id);
  require(prepare_site_directory_from_advertisements(
              configuration, current, owners, summaries, &candidate,
              &update_required) == SiteDirectoryResult::CONFLICT,
          "an uncommitted owner advertisement must fail closed");
}

void canonical_directory_prunes_groups_removed_from_configuration() {
  PlatformIdentity local{};
  ParticipantRegistry participants{};
  ParticipantReadinessRegistry readiness{};
  auto configuration =
      projection_configuration(&local, &participants, &readiness);
  GroupCoordinator coordinator{};
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  GroupHeartbeat remote{};
  remote.participant_id = "namo-node-a";
  remote.group_id = "group-main";
  remote.boot_id = "boot-a";
  remote.sequence = 1;
  remote.term = 1;
  remote.lease_ms = GroupCoordinator::DEFAULT_LEASE_MS;
  require(coordinator.accept_heartbeat(
              participants, readiness, &configuration, remote, 1000) ==
              GroupHeartbeatResult::OK,
          "pruning Group peer heartbeat was not admitted");
  coordinator.update(local, participants, readiness, &configuration,
                     "namo-node-a", 1000);
  SiteDirectorySnapshot mirror{};
  require(migrate_site_configuration_to_directory(configuration, &mirror) ==
              SiteDirectoryResult::OK,
          "pruning directory migration failed");
  SiteDirectorySnapshot current{};
  bool update_required = false;
  require(prepare_legacy_single_floorplan_projection(
              configuration, coordinator, mirror, &current,
              &update_required) == SiteDirectoryResult::OK &&
              current.group_count() == 1,
          "pruning projection fixture failed");
  current.compatibility_mirror = false;
  current.legacy_single_floorplan_projection = false;

  SiteGroupSummaryCache summaries{};
  SiteGroupSummaryReport report{};
  report.participant_id = current.groups[0].leader_node_id;
  report.group_id = current.groups[0].group_id;
  report.assignment_revision = current.groups[0].assignment_revision;
  report.term = 1;
  report.lease_ms = 6000;
  report.participants_expected = current.groups[0].member_count;
  report.participants_online = current.groups[0].member_count;
  require(summaries.accept(current, report, 1000) ==
              SiteGroupSummaryResult::OK,
          "stale Group summary fixture failed");
  for (auto &member : configuration.members)
    member.spatial_profile = {};
  configuration.floorplan = {};

  SiteFloorplanOwnerCache owners{};
  SiteDirectorySnapshot candidate{};
  require(prepare_site_directory_from_advertisements(
              configuration, current, owners, summaries, &candidate,
              &update_required) == SiteDirectoryResult::OK &&
              update_required && candidate.group_count() == 0 &&
              candidate.floorplan_count() == 0,
          "removed Groups and their unregistered Floorplans must be pruned");
}

}  // namespace

void run_site_directory_tests() {
  fixed_limits_and_role_boundaries_are_enforced();
  codec_round_trip_is_bounded_and_corruption_fails_closed();
  single_floorplan_configuration_migrates_without_guessing_groups();
  compatibility_mirror_tracks_legacy_boots_without_overwriting_canonical_state();
  foreign_canonical_directory_is_rebased_to_the_current_site();
  committed_single_floorplan_state_promotes_without_guessing();
  incomplete_or_changed_topology_fails_closed();
  store_keeps_the_last_commit_and_repairs_a_damaged_active_slot();
  failed_commit_does_not_publish_the_new_slot();
  seven_member_handover_directory_is_atomic_across_reboot();
  authenticated_advertisements_promote_the_legacy_projection();
  committed_owner_handover_updates_the_canonical_directory();
  canonical_directory_prunes_groups_removed_from_configuration();
}
