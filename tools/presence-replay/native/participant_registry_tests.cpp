#include "participant_enrollment.h"
#include "participant_registry.h"
#include "participant_registry_store.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using esphome::radar_api_server::ParticipantMembership;
using esphome::radar_api_server::ParticipantEnrollment;
using esphome::radar_api_server::ParticipantEnrollmentResult;
using esphome::radar_api_server::ParticipantRecord;
using esphome::radar_api_server::ParticipantRegistry;
using esphome::radar_api_server::ParticipantRegistryResult;
using esphome::radar_api_server::ParticipantRegistryCodec;
using esphome::radar_api_server::ParticipantRegistryCodecResult;
using esphome::radar_api_server::ParticipantRegistryCommitBlob;
using esphome::radar_api_server::ParticipantRegistrySnapshot;
using esphome::radar_api_server::ParticipantRegistryStorageBackend;
using esphome::radar_api_server::ParticipantRegistryStorageBlob;
using esphome::radar_api_server::ParticipantRegistryStore;
using esphome::radar_api_server::ParticipantRegistryStoreResult;
using esphome::radar_api_server::ParticipantReadinessConfig;
using esphome::radar_api_server::ParticipantReadinessSnapshot;
using esphome::radar_api_server::PARTICIPANT_CAP_MOTION;
using esphome::radar_api_server::PARTICIPANT_CAP_POINT_TRACKS;
using esphome::radar_api_server::PARTICIPANT_CAP_PRESENCE;
using esphome::radar_api_server::ParticipantTrust;
using esphome::radar_api_server::ParticipantType;

namespace {

class FakeRegistryBackend : public ParticipantRegistryStorageBackend {
 public:
  bool load_participant_registry_slot(uint8_t slot,
                                      ParticipantRegistryStorageBlob *blob) override {
    if (slot >= 2 || !slot_present[slot])
      return false;
    *blob = slots[slot];
    return true;
  }

  bool save_participant_registry_slot(
      uint8_t slot, const ParticipantRegistryStorageBlob &blob) override {
    events.push_back("save_slot_" + std::to_string(slot));
    if (slot >= 2 || !slot_save_succeeds)
      return false;
    slots[slot] = blob;
    slot_present[slot] = true;
    return true;
  }

  bool load_participant_registry_commit(ParticipantRegistryCommitBlob *blob) override {
    if (!commit_present)
      return false;
    *blob = commit;
    return true;
  }

  bool save_participant_registry_commit(
      const ParticipantRegistryCommitBlob &blob) override {
    events.push_back("save_commit");
    if (!commit_save_succeeds)
      return false;
    commit = blob;
    commit_present = true;
    return true;
  }

  bool sync() override {
    events.push_back("sync");
    ++sync_calls;
    return fail_sync_call == 0 || sync_calls != fail_sync_call;
  }

  ParticipantRegistryStorageBlob slots[2]{};
  ParticipantRegistryCommitBlob commit{};
  bool slot_present[2]{false, false};
  bool commit_present{false};
  bool slot_save_succeeds{true};
  bool commit_save_succeeds{true};
  int fail_sync_call{0};
  int sync_calls{0};
  std::vector<std::string> events;
};

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "participant_registry_tests: %s\n", message);
  std::exit(1);
}

void classify_and_activate(ParticipantRegistry &registry, const char *id,
                           ParticipantType type, ParticipantTrust trust) {
  require(registry.discover(id, type) == ParticipantRegistryResult::OK,
          "participant discovery must succeed");
  require(registry.classify(id, trust) == ParticipantRegistryResult::OK,
          "compatible trust classification must succeed");
  require(registry.activate(id) == ParticipantRegistryResult::OK,
          "trusted participant activation must succeed");
}

ParticipantReadinessSnapshot ready_spatial_participant(const char *participant_id) {
  ParticipantReadinessSnapshot snapshot{};
  snapshot.generation = 1;
  ParticipantReadinessConfig config{};
  config.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                            PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;
  config.max_targets = 3;
  std::strcpy(config.floorplan_id, "floor-main");
  std::strcpy(config.space_id, "room-main");
  std::strcpy(config.group_id, "fusion-main");
  config.geometry_version = 1;
  config.local_pose_version = 1;
  config.pose_version = 1;
  config.calibration_version = 1;
  config.transform.residual_mm = 0;
  config.transform.sample_count = 1;
  config.transform.valid = true;
  require(snapshot.registry.configure(participant_id, config) ==
              esphome::radar_api_server::ParticipantReadinessResult::OK,
          "the readiness fixture must accept a complete spatial profile");
  return snapshot;
}

void discovery_is_bounded_and_does_not_evict() {
  ParticipantRegistry registry;
  for (std::size_t index = 0; index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    const std::string id = "node-" + std::to_string(index);
    classify_and_activate(registry, id.c_str(), ParticipantType::NAMO_NODE,
                          ParticipantTrust::OFFICIAL_VERIFIED);
  }
  require(registry.discover("node-19", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::FULL,
          "the twentieth remote participant must be rejected");
  require(registry.size() == ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
              registry.connected_count() ==
                  ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
              registry.active_count() == ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
              registry.site_size() == ParticipantRegistry::MAX_PARTICIPANTS &&
              registry.site_active_count() == ParticipantRegistry::MAX_PARTICIPANTS &&
              registry.find("node-0") != nullptr,
          "the local node plus nineteen peers must fill the Site without eviction");
}

void ids_are_stable_and_validated() {
  ParticipantRegistry registry;
  require(registry.discover(nullptr, ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::INVALID_ID,
          "null ids must be rejected");
  require(registry.discover("-bad", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::INVALID_ID,
          "ids must start with an alphanumeric character");
  require(registry.discover("bad id", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::INVALID_ID,
          "ids must reject unsupported characters");
  const std::string too_long(ParticipantRecord::MAX_ID_LENGTH + 1, 'a');
  require(registry.discover(too_long.c_str(), ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::INVALID_ID,
          "ids longer than fixed storage must be rejected");

  require(registry.discover("node.room_1:west", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "documented stable id separators must be accepted");
  require(registry.discover("node.room_1:west", ParticipantType::SATELLITE) ==
              ParticipantRegistryResult::ALREADY_EXISTS,
          "rediscovery must not replace an existing identity");
  require(registry.find("node.room_1:west")->type == ParticipantType::NAMO_NODE,
          "participant type must remain immutable after discovery");
}

void unknown_and_incompatible_trust_stay_quarantined() {
  ParticipantRegistry registry;
  require(registry.discover("unknown-1", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "discovery must create a candidate");
  require(registry.activate("unknown-1") == ParticipantRegistryResult::INVALID_STATE,
          "unknown trust must not become active");
  require(registry.classify("unknown-1", ParticipantTrust::SATELLITE) ==
              ParticipantRegistryResult::INCOMPATIBLE_TRUST,
          "NAMO nodes must not use satellite trust");
  const auto *entry = registry.find("unknown-1");
  require(entry->membership == ParticipantMembership::QUARANTINED &&
              !entry->leader_eligible && registry.connected_count() == 1,
          "unknown candidates must remain quarantined without authority");
}

void activation_applies_leader_rules() {
  ParticipantRegistry registry;
  classify_and_activate(registry, "official-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  require(registry.find("official-1")->leader_eligible,
          "approved official nodes must be leader eligible by default");

  classify_and_activate(registry, "community-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::COMMUNITY_UNVERIFIED);
  require(!registry.find("community-1")->leader_eligible,
          "community nodes must not get implicit leader permission");

  classify_and_activate(registry, "community-2", ParticipantType::NAMO_NODE,
                        ParticipantTrust::COMMUNITY_UNVERIFIED);
  require(registry.set_community_leader_eligible("community-2", true) ==
              ParticipantRegistryResult::OK,
          "community leader permission must be a separate operation");
  require(registry.find("community-2")->leader_eligible,
          "community leader permission must require explicit opt-in");

  classify_and_activate(registry, "satellite-1", ParticipantType::SATELLITE,
                        ParticipantTrust::SATELLITE);
  require(!registry.find("satellite-1")->leader_eligible,
          "satellites must never become leader eligible");
  require(registry.set_community_leader_eligible("satellite-1", true) ==
              ParticipantRegistryResult::INVALID_STATE,
          "satellite leader requests must be rejected");
}

void lifecycle_clears_authority_and_reuses_slots() {
  ParticipantRegistry registry;
  classify_and_activate(registry, "node-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  require(registry.forget("node-1") == ParticipantRegistryResult::INVALID_STATE,
          "active participants must not be removed directly");
  require(registry.quarantine("node-1") == ParticipantRegistryResult::OK &&
              !registry.find("node-1")->leader_eligible,
          "quarantine must clear leader authority");
  require(registry.activate("node-1") == ParticipantRegistryResult::OK,
          "trusted quarantined participants may be activated again");
  require(registry.revoke("node-1") == ParticipantRegistryResult::OK &&
              registry.connected_count() == 0 && registry.active_count() == 0 &&
              !registry.find("node-1")->leader_eligible,
          "revocation must remove active status and authority");
  require(registry.activate("node-1") == ParticipantRegistryResult::INVALID_STATE,
          "revoked participants must not reactivate in place");
  require(registry.forget("node-1") == ParticipantRegistryResult::OK &&
              registry.size() == 0,
          "explicit forget must release a non-active slot");
}

std::size_t encode_snapshot(
    const ParticipantRegistrySnapshot &snapshot,
    std::array<uint8_t, ParticipantRegistryCodec::MAX_ENCODED_SIZE> *storage) {
  std::size_t encoded_size = 0;
  require(ParticipantRegistryCodec::encode(snapshot, storage->data(), storage->size(),
                                           &encoded_size) ==
              ParticipantRegistryCodecResult::OK,
          "registry snapshot encoding must succeed");
  return encoded_size;
}

void registry_codec_round_trips_lifecycle_and_authority() {
  ParticipantRegistrySnapshot source{};
  source.generation = 7;
  classify_and_activate(source.registry, "official-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  classify_and_activate(source.registry, "community-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::COMMUNITY_UNVERIFIED);
  require(source.registry.set_community_leader_eligible("community-1", true) ==
              ParticipantRegistryResult::OK,
          "the fixture must grant explicit community leadership");
  classify_and_activate(source.registry, "satellite-1", ParticipantType::SATELLITE,
                        ParticipantTrust::SATELLITE);
  require(source.registry.discover("pending-1", ParticipantType::NAMO_NODE) ==
              ParticipantRegistryResult::OK,
          "the fixture must contain a quarantined candidate");
  require(source.registry.discover("revoked-1", ParticipantType::SATELLITE) ==
                  ParticipantRegistryResult::OK &&
              source.registry.classify("revoked-1", ParticipantTrust::SATELLITE) ==
                  ParticipantRegistryResult::OK &&
              source.registry.revoke("revoked-1") == ParticipantRegistryResult::OK,
          "the fixture must contain a revoked satellite");

  std::array<uint8_t, ParticipantRegistryCodec::MAX_ENCODED_SIZE> storage{};
  const std::size_t encoded_size = encode_snapshot(source, &storage);
  ParticipantRegistrySnapshot decoded{};
  require(ParticipantRegistryCodec::decode(storage.data(), storage.size(), &decoded) ==
              ParticipantRegistryCodecResult::OK,
          "a complete registry snapshot must decode");
  require(decoded.generation == 7 && decoded.registry.size() == 5 &&
              decoded.registry.active_count() == 3,
          "registry generation and lifecycle counts must survive reboot");
  require(decoded.registry.find("official-1")->leader_eligible &&
              decoded.registry.find("community-1")->leader_eligible &&
              !decoded.registry.find("satellite-1")->leader_eligible &&
              decoded.registry.find("pending-1")->membership ==
                  ParticipantMembership::QUARANTINED &&
              decoded.registry.find("revoked-1")->membership ==
                  ParticipantMembership::REVOKED,
          "trust, membership, and leader authority must round-trip exactly");
  require(encoded_size <= storage.size(),
          "the encoded registry must stay inside its fixed storage budget");
}

void registry_codec_handles_capacity_corruption_and_versions() {
  ParticipantRegistrySnapshot source{};
  source.generation = 1;
  for (std::size_t index = 0; index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS;
       ++index) {
    const std::string id = "node-" + std::to_string(index);
    classify_and_activate(source.registry, id.c_str(), ParticipantType::NAMO_NODE,
                          ParticipantTrust::OFFICIAL_VERIFIED);
  }
  std::array<uint8_t, ParticipantRegistryCodec::MAX_ENCODED_SIZE> storage{};
  const std::size_t encoded_size = encode_snapshot(source, &storage);
  ParticipantRegistrySnapshot decoded{};
  require(ParticipantRegistryCodec::decode(storage.data(), storage.size(), &decoded) ==
                  ParticipantRegistryCodecResult::OK &&
              decoded.registry.site_size() == ParticipantRegistry::MAX_PARTICIPANTS,
          "the local node plus nineteen persisted peers must fill the Site exactly");

  auto corrupted = storage;
  corrupted[ParticipantRegistryCodec::HEADER_SIZE + 2] ^= 0x01;
  require(ParticipantRegistryCodec::decode(corrupted.data(), corrupted.size(), &decoded) ==
              ParticipantRegistryCodecResult::CORRUPT,
          "registry payload corruption must fail checksum or state validation");

  auto unsupported = storage;
  unsupported[4] = 2;
  unsupported[5] = 0;
  require(ParticipantRegistryCodec::decode(unsupported.data(), unsupported.size(), &decoded) ==
              ParticipantRegistryCodecResult::UNSUPPORTED_VERSION,
          "future registry storage versions must not be guessed");
  require(ParticipantRegistryCodec::decode(storage.data(), encoded_size - 1, &decoded) ==
              ParticipantRegistryCodecResult::CORRUPT,
          "truncated registry records must fail closed");

  auto dirty_padding = storage;
  dirty_padding[encoded_size] = 0;
  require(ParticipantRegistryCodec::decode(dirty_padding.data(), dirty_padding.size(), &decoded) ==
              ParticipantRegistryCodecResult::CORRUPT,
          "unexpected bytes after the checksum must be rejected");
}

void registry_store_commits_updates_without_losing_the_previous_generation() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistrySnapshot snapshot{};
  require(store.load(backend, &snapshot) ==
              ParticipantRegistryStoreResult::EMPTY,
          "a device without peers must begin with empty registry storage");

  ParticipantRegistry empty{};
  require(store.create(backend, empty, &snapshot) ==
                  ParticipantRegistryStoreResult::CREATED &&
              snapshot.generation == 1 && snapshot.registry.size() == 0,
          "first boot must persist an empty generation-one registry");
  require(backend.events ==
              std::vector<std::string>{"save_slot_0", "sync", "save_commit", "sync"},
          "the registry slot must become durable before its commit pointer");

  ParticipantRegistry one_peer{};
  classify_and_activate(one_peer, "node-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  require(store.update(backend, one_peer, &snapshot) ==
                  ParticipantRegistryStoreResult::UPDATED &&
              snapshot.generation == 2,
          "a registry update must advance generation through the inactive slot");
  require(store.load(backend, &snapshot) ==
                  ParticipantRegistryStoreResult::LOADED &&
              snapshot.generation == 2 && snapshot.registry.find("node-1") != nullptr,
          "the commit pointer must select the complete new registry");

  ParticipantRegistry two_peers = one_peer;
  classify_and_activate(two_peers, "node-2", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  backend.commit_save_succeeds = false;
  require(store.update(backend, two_peers, &snapshot) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "a failed commit switch must report a storage failure");
  require(store.load(backend, &snapshot) ==
                  ParticipantRegistryStoreResult::LOADED &&
              snapshot.generation == 2 && snapshot.registry.size() == 1,
          "an uncommitted inactive slot must never replace the prior registry");

  backend.commit_save_succeeds = true;
  backend.slot_save_succeeds = false;
  require(store.update(backend, two_peers, &snapshot) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "a failed inactive-slot write must abort the registry update");
  require(store.load(backend, &snapshot) ==
                  ParticipantRegistryStoreResult::LOADED &&
              snapshot.generation == 2 && snapshot.registry.size() == 1,
          "a failed slot write must preserve the committed registry");
}

void registry_store_repairs_only_unambiguous_commits() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistry empty{};
  backend.commit_save_succeeds = false;
  ParticipantRegistrySnapshot snapshot{};
  require(store.create(backend, empty, &snapshot) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "an interrupted initial commit must be reported");
  backend.commit_save_succeeds = true;
  require(store.load(backend, &snapshot) ==
                  ParticipantRegistryStoreResult::COMMIT_REPAIRED &&
              snapshot.generation == 1,
          "one valid registry slot may repair a missing initial commit");

  ParticipantRegistry one_peer{};
  classify_and_activate(one_peer, "node-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  require(store.update(backend, one_peer, &snapshot) ==
              ParticipantRegistryStoreResult::UPDATED,
          "a second complete generation must be writable");
  backend.commit_present = false;
  require(store.load(backend, &snapshot) ==
              ParticipantRegistryStoreResult::CORRUPT,
          "two valid registry slots without a commit are ambiguous and must fail closed");
}

void registry_store_reloads_the_only_valid_slot_after_a_corrupt_peer() {
  ParticipantRegistry empty{};

  FakeRegistryBackend first_valid_backend;
  ParticipantRegistryStore first_valid_store;
  ParticipantRegistrySnapshot snapshot{};
  first_valid_backend.commit_save_succeeds = false;
  require(first_valid_store.create(first_valid_backend, empty, &snapshot) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "the first valid slot fixture must stop before committing");
  first_valid_backend.slots[1] = first_valid_backend.slots[0];
  first_valid_backend.slots[1].bytes[ParticipantRegistryCodec::HEADER_SIZE] ^= 0x01;
  first_valid_backend.slot_present[1] = true;
  first_valid_backend.commit_save_succeeds = true;
  require(first_valid_store.load(first_valid_backend, &snapshot) ==
                  ParticipantRegistryStoreResult::COMMIT_REPAIRED &&
              snapshot.generation == 1 && snapshot.registry.size() == 0,
          "a valid first slot must be reloaded after the corrupt second slot reuses scratch storage");

  FakeRegistryBackend second_valid_backend;
  ParticipantRegistryStore second_valid_store;
  second_valid_backend.commit_save_succeeds = false;
  require(second_valid_store.create(second_valid_backend, empty, &snapshot) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "the second valid slot fixture must stop before committing");
  second_valid_backend.slots[1] = second_valid_backend.slots[0];
  second_valid_backend.slot_present[1] = true;
  second_valid_backend.slots[0].bytes[ParticipantRegistryCodec::HEADER_SIZE] ^= 0x01;
  second_valid_backend.commit_save_succeeds = true;
  require(second_valid_store.load(second_valid_backend, &snapshot) ==
                  ParticipantRegistryStoreResult::COMMIT_REPAIRED &&
              snapshot.generation == 1 && snapshot.registry.size() == 0,
          "a valid second slot must repair a corrupt first slot with the same bounded workspace");
}

void corrupt_registry_storage_does_not_replace_runtime_state() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistry one_peer{};
  classify_and_activate(one_peer, "node-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  ParticipantRegistrySnapshot persisted{};
  require(store.create(backend, one_peer, &persisted) ==
              ParticipantRegistryStoreResult::CREATED,
          "the corruption fixture must be persisted");
  backend.slots[0].bytes[ParticipantRegistryCodec::HEADER_SIZE + 2] ^= 0x01;

  ParticipantRegistrySnapshot runtime{};
  runtime.generation = 99;
  classify_and_activate(runtime.registry, "runtime-safe", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  require(store.load(backend, &runtime) ==
              ParticipantRegistryStoreResult::CORRUPT,
          "corrupt committed registry storage must fail closed");
  require(runtime.generation == 99 && runtime.registry.find("runtime-safe") != nullptr &&
              runtime.registry.find("node-1") == nullptr,
          "a failed load must not partially replace the caller's standalone runtime state");
}

void registry_store_migrates_legacy_storage_without_losing_authority() {
  FakeRegistryBackend legacy_backend;
  ParticipantRegistryStore legacy_store;
  ParticipantRegistry legacy_registry{};
  classify_and_activate(legacy_registry, "node-legacy", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  ParticipantRegistrySnapshot legacy_snapshot{};
  require(legacy_store.create(legacy_backend, legacy_registry, &legacy_snapshot) ==
              ParticipantRegistryStoreResult::CREATED,
          "the legacy registry fixture must be committed before migration");

  FakeRegistryBackend platform_backend;
  ParticipantRegistryStore migration_store;
  ParticipantRegistrySnapshot migrated{};
  require(migration_store.load_or_migrate(platform_backend, &legacy_backend,
                                          &migrated) ==
                  ParticipantRegistryStoreResult::MIGRATED &&
              migrated.generation == 1 &&
              migrated.registry.find("node-legacy") != nullptr &&
              migrated.registry.find("node-legacy")->membership ==
                  ParticipantMembership::ACTIVE,
          "migration must preserve the committed participant authority");

  ParticipantRegistryStore reboot_store;
  ParticipantRegistrySnapshot rebooted{};
  require(reboot_store.load(platform_backend, &rebooted) ==
                  ParticipantRegistryStoreResult::LOADED &&
              rebooted.registry.find("node-legacy") != nullptr,
          "the migrated registry must load without consulting legacy storage");

  legacy_backend.commit_save_succeeds = false;
  ParticipantRegistrySnapshot preferred{};
  require(reboot_store.load_or_migrate(platform_backend, &legacy_backend,
                                       &preferred) ==
                  ParticipantRegistryStoreResult::LOADED &&
              preferred.registry.find("node-legacy") != nullptr,
          "valid platform storage must take precedence over a broken legacy copy");
}

void registry_store_failed_migration_keeps_legacy_storage_bootable() {
  FakeRegistryBackend legacy_backend;
  ParticipantRegistryStore legacy_store;
  ParticipantRegistry legacy_registry{};
  classify_and_activate(legacy_registry, "node-legacy", ParticipantType::NAMO_NODE,
                        ParticipantTrust::OFFICIAL_VERIFIED);
  ParticipantRegistrySnapshot legacy_snapshot{};
  require(legacy_store.create(legacy_backend, legacy_registry, &legacy_snapshot) ==
              ParticipantRegistryStoreResult::CREATED,
          "the failed migration fixture must have committed legacy storage");

  FakeRegistryBackend platform_backend;
  platform_backend.slot_save_succeeds = false;
  ParticipantRegistryStore migration_store;
  ParticipantRegistrySnapshot runtime{};
  require(migration_store.load_or_migrate(platform_backend, &legacy_backend,
                                          &runtime) ==
              ParticipantRegistryStoreResult::STORAGE_FAILED,
          "migration must report a failed platform write");

  ParticipantRegistryStore recovery_store;
  ParticipantRegistrySnapshot recovered{};
  require(recovery_store.load(legacy_backend, &recovered) ==
                  ParticipantRegistryStoreResult::LOADED &&
              recovered.registry.find("node-legacy") != nullptr,
          "a failed migration must leave the legacy transaction readable");
}

void full_registry_survives_repeated_transactional_updates() {
  ParticipantRegistry full{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    const std::string id = "node-" + std::to_string(index);
    classify_and_activate(full, id.c_str(), ParticipantType::NAMO_NODE,
                          ParticipantTrust::OFFICIAL_VERIFIED);
  }

  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistrySnapshot runtime{};
  require(store.create(backend, full, &runtime) ==
              ParticipantRegistryStoreResult::CREATED,
          "the repeated registry fixture must begin at full capacity");
  for (std::size_t iteration = 0; iteration < 128; ++iteration) {
    const bool activate = (iteration % 2) != 0;
    const auto mutation = activate ? runtime.registry.activate("node-0")
                                   : runtime.registry.quarantine("node-0");
    require(mutation == ParticipantRegistryResult::OK &&
                store.update(backend, runtime.registry, &runtime) ==
                    ParticipantRegistryStoreResult::UPDATED,
            "repeated full-registry updates must remain transactional");
  }

  ParticipantRegistryStore reboot_store;
  ParticipantRegistrySnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) ==
                  ParticipantRegistryStoreResult::LOADED &&
              rebooted.registry.size() ==
                  ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
              rebooted.registry.find("node-0")->membership ==
                  ParticipantMembership::ACTIVE,
          "the latest full registry generation must survive repeated writes");
}

void enrollment_window_is_bounded_and_wrap_safe() {
  ParticipantEnrollment enrollment;
  ParticipantRegistrySnapshot snapshot{};

  require(enrollment.observe_candidate(snapshot, "node-1", ParticipantType::NAMO_NODE, 10) ==
              ParticipantEnrollmentResult::WINDOW_CLOSED,
          "candidate discovery must be disabled while the pairing window is closed");
  require(enrollment.open_window(10, 0) == ParticipantEnrollmentResult::INVALID_ARGUMENT &&
              enrollment.open_window(10, ParticipantEnrollment::MAX_PAIRING_WINDOW_MS + 1) ==
                  ParticipantEnrollmentResult::INVALID_ARGUMENT,
          "pairing windows must reject zero and unbounded durations");

  require(enrollment.open_window(10, 100) == ParticipantEnrollmentResult::OK &&
              enrollment.observe_candidate(snapshot, "node-1", ParticipantType::NAMO_NODE, 20) ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              !enrollment.has_pending_candidate(20),
          "an unavailable registry must reject candidates without affecting standalone sensing");

  snapshot.generation = 1;
  const uint32_t opened_at = 0xFFFFFFF0U;
  require(enrollment.open_window(opened_at, 100) == ParticipantEnrollmentResult::OK &&
              enrollment.window_open(20) && enrollment.window_remaining_ms(20) == 64,
          "pairing window elapsed time must remain valid across millis wraparound");
  require(enrollment.observe_candidate(snapshot, "node-1", ParticipantType::NAMO_NODE, 20) ==
                  ParticipantEnrollmentResult::OK &&
              enrollment.has_pending_candidate(20),
          "an open pairing window must retain one bounded candidate");
  require(enrollment.observe_candidate(snapshot, "node-1", ParticipantType::NAMO_NODE, 21) ==
              ParticipantEnrollmentResult::OK,
          "rediscovering the same pending candidate must be idempotent");
  require(enrollment.observe_candidate(snapshot, "node-2", ParticipantType::NAMO_NODE, 22) ==
              ParticipantEnrollmentResult::CANDIDATE_PENDING,
          "a second candidate must not replace the pending candidate");
  require(enrollment.observe_candidate(snapshot, "node-1", ParticipantType::NAMO_NODE, 100) ==
                  ParticipantEnrollmentResult::WINDOW_EXPIRED &&
              !enrollment.window_open(100) && enrollment.pending_candidate(100) == nullptr,
          "window expiry must clear the unapproved candidate");
  require(enrollment.window_remaining_ms(100) == 0,
          "closed or expired windows must never report stale remaining time");
}

void enrollment_approval_enforces_trust_and_persists() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistrySnapshot runtime{};
  ParticipantRegistry empty{};
  require(store.create(backend, empty, &runtime) == ParticipantRegistryStoreResult::CREATED,
          "the enrollment fixture must start with a committed empty registry");

  ParticipantEnrollment enrollment;
  require(enrollment.open_window(100, 1000) == ParticipantEnrollmentResult::OK &&
              enrollment.observe_candidate(runtime, "community-1", ParticipantType::NAMO_NODE,
                                             110) == ParticipantEnrollmentResult::OK,
          "the NAMO candidate fixture must enter the pairing window");
  require(enrollment.approve_candidate(backend, store, &runtime,
                                       ParticipantTrust::OFFICIAL_VERIFIED, 120) ==
                  ParticipantEnrollmentResult::ATTESTATION_REQUIRED &&
              runtime.generation == 1 && runtime.registry.size() == 0 &&
              enrollment.has_pending_candidate(120),
          "unverified callers must not mint official trust or consume the candidate");
  require(enrollment.approve_candidate(backend, store, &runtime,
                                       ParticipantTrust::SATELLITE, 130) ==
                  ParticipantEnrollmentResult::INCOMPATIBLE_TRUST &&
              runtime.registry.size() == 0,
          "NAMO nodes must not be approved with satellite trust");
  require(enrollment.approve_candidate(backend, store, &runtime,
                                       ParticipantTrust::COMMUNITY_UNVERIFIED, 140) ==
                   ParticipantEnrollmentResult::OK &&
              runtime.generation == 2 && runtime.registry.active_count() == 0 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::QUARANTINED &&
              runtime.registry.find("community-1")->trust ==
                  ParticipantTrust::COMMUNITY_UNVERIFIED &&
              !runtime.registry.find("community-1")->leader_eligible &&
              !enrollment.window_open(140),
          "community approval must persist trust in quarantine and close the window");
  const auto community_readiness = ready_spatial_participant("community-1");
  require(enrollment.activate(backend, store, &runtime, community_readiness,
                              "community-1") ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 3 && runtime.registry.active_count() == 1 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::ACTIVE,
          "approved community nodes must require a separate committed activation");

  require(enrollment.open_window(200, 1000) == ParticipantEnrollmentResult::OK &&
              enrollment.observe_candidate(runtime, "satellite-1", ParticipantType::SATELLITE,
                                             210) == ParticipantEnrollmentResult::OK &&
              enrollment.approve_candidate(backend, store, &runtime,
                                             ParticipantTrust::SATELLITE, 220) ==
                  ParticipantEnrollmentResult::OK,
          "satellite enrollment must use the same explicit approval path");
  const auto *satellite = runtime.registry.find("satellite-1");
  require(runtime.generation == 4 && satellite != nullptr &&
              satellite->membership == ParticipantMembership::QUARANTINED &&
              satellite->trust == ParticipantTrust::SATELLITE &&
              !satellite->leader_eligible,
          "approved satellites must persist in quarantine without Leader authority");

  ParticipantRegistryStore reboot_store;
  ParticipantRegistrySnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) == ParticipantRegistryStoreResult::LOADED &&
              rebooted.generation == runtime.generation &&
              rebooted.registry.find("community-1")->membership ==
                  ParticipantMembership::ACTIVE &&
              rebooted.registry.find("satellite-1")->membership ==
                  ParticipantMembership::QUARANTINED,
          "active and approved-quarantined participants must both restore after reboot");
}

void enrollment_storage_failure_rolls_back_runtime() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistrySnapshot runtime{};
  ParticipantRegistry empty{};
  require(store.create(backend, empty, &runtime) == ParticipantRegistryStoreResult::CREATED,
          "the rollback fixture must start with committed storage");

  ParticipantEnrollment enrollment;
  require(enrollment.open_window(10, 1000) == ParticipantEnrollmentResult::OK &&
              enrollment.observe_candidate(runtime, "community-1", ParticipantType::NAMO_NODE,
                                             20) == ParticipantEnrollmentResult::OK,
          "the rollback candidate fixture must be pending");
  backend.commit_save_succeeds = false;
  require(enrollment.approve_candidate(backend, store, &runtime,
                                       ParticipantTrust::COMMUNITY_UNVERIFIED, 30) ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              runtime.generation == 1 && runtime.registry.size() == 0 &&
              enrollment.has_pending_candidate(30),
          "a failed commit must leave runtime unchanged and keep the candidate retryable");

  backend.commit_save_succeeds = true;
  require(enrollment.approve_candidate(backend, store, &runtime,
                                       ParticipantTrust::COMMUNITY_UNVERIFIED, 40) ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 2 && runtime.registry.find("community-1") != nullptr,
          "retrying after storage recovery must commit exactly one new generation");
}

void enrollment_lifecycle_mutations_are_transactional() {
  FakeRegistryBackend backend;
  ParticipantRegistryStore store;
  ParticipantRegistry registry{};
  classify_and_activate(registry, "community-1", ParticipantType::NAMO_NODE,
                        ParticipantTrust::COMMUNITY_UNVERIFIED);
  ParticipantRegistrySnapshot runtime{};
  require(store.create(backend, registry, &runtime) == ParticipantRegistryStoreResult::CREATED,
          "the lifecycle fixture must start with one active community node");

  ParticipantEnrollment enrollment;
  const auto readiness = ready_spatial_participant("community-1");
  require(enrollment.set_community_leader_eligible(backend, store, &runtime,
                                                    "community-1", true) ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 2 && runtime.registry.find("community-1")->leader_eligible,
          "community Leader opt-in must be an explicit committed mutation");

  backend.commit_save_succeeds = false;
  require(enrollment.set_community_leader_eligible(backend, store, &runtime,
                                                    "community-1", false) ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              runtime.generation == 2 &&
              runtime.registry.find("community-1")->leader_eligible,
          "failed community Leader opt-out must preserve committed authority");
  backend.commit_save_succeeds = true;
  require(enrollment.set_community_leader_eligible(backend, store, &runtime,
                                                    "community-1", false) ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 3 &&
              !runtime.registry.find("community-1")->leader_eligible,
          "community Leader opt-out must be an explicit committed mutation");
  require(enrollment.set_community_leader_eligible(backend, store, &runtime,
                                                    "community-1", true) ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 4 && runtime.registry.find("community-1")->leader_eligible,
          "community Leader opt-in must remain reversible");

  backend.commit_save_succeeds = false;
  require(enrollment.quarantine(backend, store, &runtime, "community-1") ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              runtime.generation == 4 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::ACTIVE &&
              runtime.registry.find("community-1")->leader_eligible,
          "failed quarantine persistence must not alter live authority");
  backend.commit_save_succeeds = true;
  require(enrollment.quarantine(backend, store, &runtime, "community-1") ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 5 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::QUARANTINED &&
              !runtime.registry.find("community-1")->leader_eligible,
          "committed quarantine must clear Leader authority");
  backend.commit_save_succeeds = false;
  require(enrollment.activate(backend, store, &runtime, readiness,
                              "community-1") ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              runtime.generation == 5 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::QUARANTINED,
          "failed activation persistence must leave the live participant quarantined");
  backend.commit_save_succeeds = true;
  require(enrollment.activate(backend, store, &runtime, readiness,
                              "community-1") ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 6 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::ACTIVE &&
              !runtime.registry.find("community-1")->leader_eligible,
          "activation must be a separate committed mutation without implicit community leadership");
  require(enrollment.revoke(backend, store, &runtime, "community-1") ==
                  ParticipantEnrollmentResult::OK &&
              runtime.generation == 7 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::REVOKED,
          "revocation must persist as a separate generation");
  require(enrollment.activate(backend, store, &runtime, readiness,
                              "community-1") ==
                  ParticipantEnrollmentResult::INVALID_STATE &&
              runtime.generation == 7 &&
              runtime.registry.find("community-1")->membership ==
                  ParticipantMembership::REVOKED,
          "revoked participants must not reactivate or advance the generation");
}

void enrollment_rejects_capacity_before_pending() {
  ParticipantRegistrySnapshot full{};
  for (std::size_t index = 0; index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    const std::string id = "node-" + std::to_string(index);
    classify_and_activate(full.registry, id.c_str(), ParticipantType::NAMO_NODE,
                          ParticipantTrust::OFFICIAL_VERIFIED);
  }
  full.generation = 1;

  ParticipantEnrollment enrollment;
  require(enrollment.open_window(10, 1000) == ParticipantEnrollmentResult::OK &&
              enrollment.observe_candidate(full, "overflow", ParticipantType::NAMO_NODE, 20) ==
                  ParticipantEnrollmentResult::REGISTRY_FULL &&
              !enrollment.has_pending_candidate(20),
          "the twentieth remote candidate must be rejected without occupying pending state");
}

}  // namespace

void run_participant_registry_tests() {
  discovery_is_bounded_and_does_not_evict();
  ids_are_stable_and_validated();
  unknown_and_incompatible_trust_stay_quarantined();
  activation_applies_leader_rules();
  lifecycle_clears_authority_and_reuses_slots();
  registry_codec_round_trips_lifecycle_and_authority();
  registry_codec_handles_capacity_corruption_and_versions();
  registry_store_commits_updates_without_losing_the_previous_generation();
  registry_store_repairs_only_unambiguous_commits();
  registry_store_reloads_the_only_valid_slot_after_a_corrupt_peer();
  corrupt_registry_storage_does_not_replace_runtime_state();
  registry_store_migrates_legacy_storage_without_losing_authority();
  registry_store_failed_migration_keeps_legacy_storage_bootable();
  full_registry_survives_repeated_transactional_updates();
  enrollment_window_is_bounded_and_wrap_safe();
  enrollment_approval_enforces_trust_and_persists();
  enrollment_storage_failure_rolls_back_runtime();
  enrollment_lifecycle_mutations_are_transactional();
  enrollment_rejects_capacity_before_pending();
}
