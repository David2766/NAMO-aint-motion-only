#include "participant_enrollment.h"
#include "participant_readiness.h"
#include "participant_registry_store.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "participant_readiness_tests: %s\n", message);
  std::exit(1);
}

class FakeReadinessBackend : public ParticipantReadinessStorageBackend {
 public:
  bool load_participant_readiness_slot(uint8_t slot, StorageBlob *blob) override {
    if (slot >= 2 || !slot_present[slot])
      return false;
    *blob = slots[slot];
    return true;
  }
  bool save_participant_readiness_slot(uint8_t slot,
                                       const StorageBlob &blob) override {
    if (slot >= 2 || !slot_save_succeeds)
      return false;
    slots[slot] = blob;
    slot_present[slot] = true;
    return true;
  }
  bool load_participant_readiness_commit(CommitBlob *blob) override {
    if (!commit_present)
      return false;
    *blob = commit;
    return true;
  }
  bool save_participant_readiness_commit(const CommitBlob &blob) override {
    if (!commit_save_succeeds)
      return false;
    commit = blob;
    commit_present = true;
    return true;
  }
  bool sync() override { return sync_succeeds; }

  StorageBlob slots[2]{};
  CommitBlob commit{};
  bool slot_present[2]{false, false};
  bool commit_present{false};
  bool slot_save_succeeds{true};
  bool commit_save_succeeds{true};
  bool sync_succeeds{true};
};

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
    if (!commit_save_succeeds)
      return false;
    commit = blob;
    commit_present = true;
    return true;
  }
  bool sync() override { return sync_succeeds; }

  ParticipantRegistryStorageBlob slots[2]{};
  ParticipantRegistryCommitBlob commit{};
  bool slot_present[2]{false, false};
  bool commit_present{false};
  bool slot_save_succeeds{true};
  bool commit_save_succeeds{true};
  bool sync_succeeds{true};
};

uint32_t test_fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

void write_test_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  data[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  data[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

ParticipantReadinessConfig spatial_config(uint32_t pose = 1,
                                          uint32_t calibration = 1) {
  ParticipantReadinessConfig config{};
  config.capability_flags = PARTICIPANT_CAP_POINT_TRACKS |
                            PARTICIPANT_CAP_VELOCITY |
                            PARTICIPANT_CAP_RANGE |
                            PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;
  config.max_targets = 3;
  std::strcpy(config.floorplan_id, "floor-main");
  std::strcpy(config.space_id, "room-main");
  std::strcpy(config.group_id, "fusion-main");
  config.geometry_version = 4;
  config.local_pose_version = 1;
  config.pose_version = pose;
  config.calibration_version = calibration;
  if (calibration != 0) {
    config.transform.origin_x_mm = 1200;
    config.transform.origin_y_mm = 800;
    config.transform.rotation_cdeg = 1500;
    config.transform.residual_mm = 45;
    config.transform.sample_count = 2;
    config.transform.valid = true;
  }
  return config;
}

ParticipantReadinessConfig presence_only_config() {
  ParticipantReadinessConfig config{};
  config.capability_flags = PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION;
  std::strcpy(config.floorplan_id, "floor-main");
  std::strcpy(config.space_id, "room-main");
  std::strcpy(config.group_id, "fusion-main");
  return config;
}

ParticipantRegistry active_registry(const char *participant_id,
                                    ParticipantType type = ParticipantType::NAMO_NODE) {
  ParticipantRegistry registry{};
  require(registry.discover(participant_id, type) == ParticipantRegistryResult::OK,
          "participant discovery fixture must succeed");
  const ParticipantTrust trust = type == ParticipantType::SATELLITE
                                     ? ParticipantTrust::SATELLITE
                                     : ParticipantTrust::COMMUNITY_UNVERIFIED;
  require(registry.classify(participant_id, trust) == ParticipantRegistryResult::OK &&
              registry.activate(participant_id) == ParticipantRegistryResult::OK,
          "participant activation fixture must succeed");
  return registry;
}

void readiness_profiles_distinguish_spatial_and_presence_only_sources() {
  ParticipantReadinessRegistry registry{};
  auto incomplete = spatial_config(0, 0);
  require(registry.configure("node-1", incomplete) == ParticipantReadinessResult::OK,
          "incomplete spatial versions must remain persistable for setup");
  const auto *node = registry.find("node-1");
  require(!ParticipantReadinessRegistry::ready(node) &&
              ParticipantReadinessRegistry::missing_requirements(node) ==
                  (PARTICIPANT_MISSING_POSE | PARTICIPANT_MISSING_CALIBRATION),
          "spatial sources must require pose and calibration before activation");

  require(registry.configure("satellite-1", presence_only_config()) ==
                  ParticipantReadinessResult::OK &&
              ParticipantReadinessRegistry::ready(registry.find("satellite-1")),
          "presence-only satellites must not require a fabricated spatial pose");

  auto invalid = presence_only_config();
  invalid.max_targets = 1;
  require(registry.configure("invalid-1", invalid) != ParticipantReadinessResult::OK,
          "presence-only sources must not claim point targets");

  ParticipantReadinessRegistry capacity{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    char participant_id[24]{};
    std::snprintf(participant_id, sizeof(participant_id), "satellite-%u",
                  static_cast<unsigned>(index));
    require(capacity.configure(participant_id, presence_only_config()) ==
                ParticipantReadinessResult::OK,
            "all nineteen remote readiness slots must be usable");
  }
  require(capacity.configure("satellite-overflow", presence_only_config()) ==
              ParticipantReadinessResult::FULL,
          "readiness must reject the twentieth remote profile without eviction");
}

void spatial_transforms_validate_and_apply_the_approved_correction() {
  SpatialTransform2D transform{};
  transform.origin_x_mm = 1000;
  transform.origin_y_mm = 2000;
  transform.rotation_cdeg = 9000;
  transform.residual_mm = 80;
  transform.sample_count = 2;
  transform.valid = true;
  require(valid_spatial_transform(transform),
          "a bounded two-sample transform must be valid");

  const auto point = apply_spatial_transform(transform, 500.0f, 250.0f);
  require(std::abs(point.x - 1250.0f) < 0.1f &&
              std::abs(point.y - 2500.0f) < 0.1f,
          "the approved transform must preserve the radar-to-floorplan axis convention");

  auto invalid = spatial_config();
  invalid.transform.residual_mm = 1001;
  require(ParticipantReadinessRegistry{}.configure("node-invalid", invalid) ==
              ParticipantReadinessResult::INVALID_ASSIGNMENT,
          "a transform above the residual limit must not become ready");

  invalid = spatial_config();
  invalid.transform.sample_count = 0;
  require(ParticipantReadinessRegistry{}.configure("node-invalid", invalid) ==
              ParticipantReadinessResult::INVALID_ASSIGNMENT,
          "a calibrated transform must contain at least one approved sample");

  invalid = spatial_config();
  invalid.transform.valid = false;
  ParticipantReadinessRegistry legacy_compatible{};
  require(legacy_compatible.configure("node-legacy", invalid) ==
                  ParticipantReadinessResult::OK &&
              ParticipantReadinessRegistry::ready(
                  legacy_compatible.find("node-legacy")),
          "an existing calibrated profile without transform data must remain ready");
}

void spatial_readiness_detects_geometry_and_pose_changes() {
  ParticipantReadinessRegistry registry{};
  require(registry.configure("node-1", spatial_config()) ==
              ParticipantReadinessResult::OK,
          "spatial invalidation fixture must be valid");
  const auto *record = registry.find("node-1");
  require(spatial_readiness_mismatch(record, true, "floor-main", 4, 1, 1) ==
              SpatialReadinessMismatch::NONE,
          "matching geometry and poses must preserve readiness");
  require(spatial_readiness_mismatch(record, true, "floor-main", 5, 1, 1) ==
              SpatialReadinessMismatch::FLOORPLAN_GEOMETRY,
          "shared geometry changes must invalidate calibration");
  require(spatial_readiness_mismatch(record, true, "floor-main", 4, 2, 1) ==
              SpatialReadinessMismatch::LOCAL_POSE,
          "local radar movement must invalidate calibration");
  require(spatial_readiness_mismatch(record, true, "floor-main", 4, 1, 2) ==
              SpatialReadinessMismatch::REMOTE_POSE,
          "authenticated remote radar movement must invalidate calibration");
  require(spatial_readiness_mismatch(record, true, "floor-main", 4, 1, 0) ==
              SpatialReadinessMismatch::NONE,
          "older peers without a heartbeat pose must not be invalidated blindly");
}

void readiness_storage_round_trips_and_rolls_back_failed_commits() {
  FakeReadinessBackend backend{};
  ParticipantReadinessStore store{};
  ParticipantReadinessSnapshot runtime{};
  ParticipantReadinessRegistry empty{};
  require(store.create(backend, empty, &runtime) ==
              ParticipantReadinessStoreResult::CREATED,
          "readiness storage must create an empty committed generation");
  require(runtime.registry.configure("node-1", spatial_config()) ==
                  ParticipantReadinessResult::OK &&
              store.update(backend, runtime.registry, &runtime) ==
                  ParticipantReadinessStoreResult::UPDATED,
          "readiness storage must commit a complete profile");

  ParticipantReadinessStore reboot_store{};
  ParticipantReadinessSnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED &&
              rebooted.generation == runtime.generation &&
              ParticipantReadinessRegistry::ready(rebooted.registry.find("node-1")) &&
              rebooted.registry.find("node-1")->config.geometry_version == 4 &&
              rebooted.registry.find("node-1")->config.local_pose_version == 1 &&
              rebooted.registry.find("node-1")->config.transform.origin_x_mm == 1200 &&
              rebooted.registry.find("node-1")->config.transform.rotation_cdeg == 1500 &&
              rebooted.registry.find("node-1")->config.transform.sample_count == 2,
          "readiness profiles and approved transforms must survive reboot");

  const uint32_t committed_generation = runtime.generation;
  const auto committed_config = *runtime.registry.find("node-1");
  auto changed = spatial_config(2, 2);
  runtime.registry.configure("node-1", changed);
  backend.commit_save_succeeds = false;
  require(store.update(backend, runtime.registry, &runtime) ==
                  ParticipantReadinessStoreResult::STORAGE_FAILED &&
              runtime.generation == committed_generation,
          "a failed commit must not advance the caller generation");
  runtime.registry.configure("node-1", committed_config.config);

  ParticipantReadinessStore after_failure_store{};
  ParticipantReadinessSnapshot after_failure{};
  require(after_failure_store.load(backend, &after_failure) ==
                  ParticipantReadinessStoreResult::LOADED &&
              after_failure.registry.find("node-1")->config.pose_version == 1,
          "a failed profile commit must leave the previous generation bootable");
}

void readiness_storage_round_trips_in_external_workspace() {
  FakeReadinessBackend backend{};
  alignas(ParticipantReadinessStore)
      unsigned char workspace[sizeof(ParticipantReadinessStore)]{};

  auto *store = new (workspace) ParticipantReadinessStore();
  ParticipantReadinessSnapshot runtime{};
  ParticipantReadinessRegistry registry{};
  require(registry.configure("node-external", spatial_config()) ==
                  ParticipantReadinessResult::OK &&
              store->create(backend, registry, &runtime) ==
                  ParticipantReadinessStoreResult::CREATED,
          "externally allocated readiness workspace must persist profiles");
  const uint32_t committed_generation = runtime.generation;
  store->~ParticipantReadinessStore();

  std::memset(workspace, 0, sizeof(workspace));
  store = new (workspace) ParticipantReadinessStore();
  ParticipantReadinessSnapshot rebooted{};
  require(store->load(backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED &&
              rebooted.generation == committed_generation,
          "reconstructed external readiness workspace must load committed data");
  const auto *record = rebooted.registry.find("node-external");
  require(record != nullptr && ParticipantReadinessRegistry::ready(record) &&
              record->config.transform.origin_x_mm == 1200,
          "external readiness workspace must preserve the approved transform");
  store->~ParticipantReadinessStore();
}

void legacy_v1_spatial_profiles_load_but_require_recalibration() {
  FakeReadinessBackend backend{};
  ParticipantReadinessStore store{};
  ParticipantReadinessSnapshot runtime{};
  ParticipantReadinessRegistry registry{};
  require(registry.configure("node-v1", spatial_config()) ==
                  ParticipantReadinessResult::OK &&
              store.create(backend, registry, &runtime) ==
                  ParticipantReadinessStoreResult::CREATED,
          "legacy schema fixture must begin as one valid committed profile");

  const uint8_t slot = backend.commit[6];
  auto &blob = backend.slots[slot];
  const std::size_t text_size = static_cast<std::size_t>(blob[12]) +
                                static_cast<std::size_t>(blob[16]) +
                                static_cast<std::size_t>(blob[17]) +
                                static_cast<std::size_t>(blob[18]);
  ParticipantReadinessStorageBackend::StorageBlob legacy{};
  legacy.fill(0xFF);
  std::memcpy(legacy.data(), blob.data(), 12);
  legacy[4] = 1;
  legacy[5] = 0;
  std::memcpy(legacy.data() + 12, blob.data() + 12, 15);
  std::memcpy(legacy.data() + 27, blob.data() + 49, text_size);
  const std::size_t checksum_offset = 27 + text_size;
  write_test_u32(legacy.data() + checksum_offset,
                 test_fnv1a(legacy.data(), checksum_offset));
  blob = legacy;

  ParticipantReadinessStore reboot_store{};
  ParticipantReadinessSnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED,
          "a committed v1 readiness blob must remain readable");
  const auto *record = rebooted.registry.find("node-v1");
  require(record != nullptr && record->config.pose_version == 1 &&
              record->config.calibration_version == 1 &&
              record->config.geometry_version == 0 &&
              record->config.local_pose_version == 0 &&
              !record->config.transform.valid &&
              !ParticipantReadinessRegistry::ready(record),
          "v1 spatial readiness must load but fail closed until recalibrated");
}

void legacy_v2_spatial_profiles_load_but_require_recalibration() {
  FakeReadinessBackend backend{};
  ParticipantReadinessStore store{};
  ParticipantReadinessSnapshot runtime{};
  ParticipantReadinessRegistry registry{};
  require(registry.configure("node-v2", spatial_config()) ==
                  ParticipantReadinessResult::OK &&
              store.create(backend, registry, &runtime) ==
                  ParticipantReadinessStoreResult::CREATED,
          "v2 schema fixture must begin as one valid committed profile");

  const uint8_t slot = backend.commit[6];
  auto &blob = backend.slots[slot];
  const std::size_t text_size = static_cast<std::size_t>(blob[12]) +
                                static_cast<std::size_t>(blob[16]) +
                                static_cast<std::size_t>(blob[17]) +
                                static_cast<std::size_t>(blob[18]);
  ParticipantReadinessStorageBackend::StorageBlob legacy{};
  legacy.fill(0xFF);
  std::memcpy(legacy.data(), blob.data(), 12);
  legacy[4] = 2;
  legacy[5] = 0;
  std::memcpy(legacy.data() + 12, blob.data() + 12, 15);
  std::memcpy(legacy.data() + 27, blob.data() + 35, 14);
  std::memcpy(legacy.data() + 41, blob.data() + 49, text_size);
  const std::size_t checksum_offset = 41 + text_size;
  write_test_u32(legacy.data() + checksum_offset,
                 test_fnv1a(legacy.data(), checksum_offset));
  blob = legacy;

  ParticipantReadinessStore reboot_store{};
  ParticipantReadinessSnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED,
          "a committed v2 readiness blob must remain readable");
  const auto *record = rebooted.registry.find("node-v2");
  require(record != nullptr && record->config.pose_version == 1 &&
              record->config.calibration_version == 1 &&
              record->config.geometry_version == 0 &&
              record->config.local_pose_version == 0 &&
              record->config.transform.valid &&
              record->config.transform.origin_x_mm == 1200 &&
              !ParticipantReadinessRegistry::ready(record),
          "v2 readiness must preserve its transform but require recalibration");
}

void readiness_store_migrates_full_legacy_storage() {
  ParticipantReadinessRegistry full{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    char participant_id[24]{};
    std::snprintf(participant_id, sizeof(participant_id), "satellite-%u",
                  static_cast<unsigned>(index));
    require(full.configure(participant_id, presence_only_config()) ==
                ParticipantReadinessResult::OK,
            "the legacy readiness fixture must fill every remote slot");
  }

  FakeReadinessBackend legacy_backend{};
  ParticipantReadinessStore legacy_store{};
  ParticipantReadinessSnapshot legacy_snapshot{};
  require(legacy_store.create(legacy_backend, full, &legacy_snapshot) ==
              ParticipantReadinessStoreResult::CREATED,
          "the full legacy readiness fixture must be committed");

  FakeReadinessBackend platform_backend{};
  ParticipantReadinessStore migration_store{};
  ParticipantReadinessSnapshot migrated{};
  require(migration_store.load_or_migrate(platform_backend, &legacy_backend,
                                          &migrated) ==
                  ParticipantReadinessStoreResult::MIGRATED &&
              migrated.registry.size() ==
                  ParticipantRegistry::MAX_REMOTE_PARTICIPANTS,
          "readiness migration must preserve all nineteen profiles");

  ParticipantReadinessStore reboot_store{};
  ParticipantReadinessSnapshot rebooted{};
  require(reboot_store.load(platform_backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED &&
              rebooted.registry.find("satellite-18") != nullptr,
          "migrated readiness must reload from platform storage alone");
}

void readiness_failed_migration_keeps_legacy_storage_bootable() {
  ParticipantReadinessRegistry legacy_registry{};
  require(legacy_registry.configure("node-legacy", spatial_config()) ==
              ParticipantReadinessResult::OK,
          "the failed readiness migration fixture must be valid");
  FakeReadinessBackend legacy_backend{};
  ParticipantReadinessStore legacy_store{};
  ParticipantReadinessSnapshot legacy_snapshot{};
  require(legacy_store.create(legacy_backend, legacy_registry,
                              &legacy_snapshot) ==
              ParticipantReadinessStoreResult::CREATED,
          "the failed readiness migration fixture must be committed");

  FakeReadinessBackend platform_backend{};
  platform_backend.slot_save_succeeds = false;
  ParticipantReadinessStore migration_store{};
  ParticipantReadinessSnapshot runtime{};
  require(migration_store.load_or_migrate(platform_backend, &legacy_backend,
                                          &runtime) ==
              ParticipantReadinessStoreResult::STORAGE_FAILED,
          "readiness migration must report a failed platform write");

  ParticipantReadinessStore recovery_store{};
  ParticipantReadinessSnapshot recovered{};
  require(recovery_store.load(legacy_backend, &recovered) ==
                  ParticipantReadinessStoreResult::LOADED &&
              recovered.registry.find("node-legacy") != nullptr,
          "failed readiness migration must leave legacy storage readable");
}

void full_readiness_registry_survives_repeated_updates() {
  ParticipantReadinessRegistry full{};
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
    char participant_id[24]{};
    std::snprintf(participant_id, sizeof(participant_id), "node-%u",
                  static_cast<unsigned>(index));
    require(full.configure(participant_id, spatial_config()) ==
                ParticipantReadinessResult::OK,
            "the repeated readiness fixture must fill every remote slot");
  }

  FakeReadinessBackend backend{};
  ParticipantReadinessStore store{};
  ParticipantReadinessSnapshot runtime{};
  require(store.create(backend, full, &runtime) ==
              ParticipantReadinessStoreResult::CREATED,
          "the repeated readiness fixture must be committed");
  for (uint32_t version = 2; version <= 129; ++version) {
    require(runtime.registry.configure("node-0", spatial_config(version, version)) ==
                    ParticipantReadinessResult::OK &&
                store.update(backend, runtime.registry, &runtime) ==
                    ParticipantReadinessStoreResult::UPDATED,
            "repeated full-readiness updates must remain transactional");
  }

  ParticipantReadinessStore reboot_store{};
  ParticipantReadinessSnapshot rebooted{};
  require(reboot_store.load(backend, &rebooted) ==
                  ParticipantReadinessStoreResult::LOADED &&
              rebooted.registry.size() ==
                  ParticipantRegistry::MAX_REMOTE_PARTICIPANTS &&
              rebooted.registry.find("node-0")->config.pose_version == 129,
          "the latest readiness profile must survive repeated full writes");
}

void enrollment_requires_readiness_and_quarantines_reconfiguration() {
  FakeRegistryBackend registry_backend{};
  FakeReadinessBackend readiness_backend{};
  ParticipantRegistryStore registry_store{};
  ParticipantReadinessStore readiness_store{};
  ParticipantRegistrySnapshot participants{};
  ParticipantReadinessSnapshot readiness{};
  ParticipantRegistry trusted{};
  require(trusted.discover("node-1", ParticipantType::NAMO_NODE) ==
                  ParticipantRegistryResult::OK &&
              trusted.classify("node-1", ParticipantTrust::COMMUNITY_UNVERIFIED) ==
                  ParticipantRegistryResult::OK &&
              registry_store.create(registry_backend, trusted, &participants) ==
                  ParticipantRegistryStoreResult::CREATED,
          "enrollment fixture must begin with one trusted quarantined node");
  ParticipantReadinessRegistry empty{};
  require(readiness_store.create(readiness_backend, empty, &readiness) ==
              ParticipantReadinessStoreResult::CREATED,
          "enrollment fixture must begin with readiness persistence");

  ParticipantEnrollment enrollment{};
  require(enrollment.activate(registry_backend, registry_store, &participants,
                              readiness, "node-1") ==
                  ParticipantEnrollmentResult::NOT_READY,
          "activation must reject trusted participants without a complete profile");
  require(enrollment.configure_participant(
              registry_backend, registry_store, &participants, readiness_backend,
              readiness_store, &readiness, "node-1", spatial_config(0, 0)) ==
                  ParticipantEnrollmentResult::OK &&
              enrollment.activate(registry_backend, registry_store, &participants,
                                  readiness, "node-1") ==
                  ParticipantEnrollmentResult::NOT_READY,
          "zero pose and calibration versions must keep spatial nodes quarantined");
  require(enrollment.configure_participant(
              registry_backend, registry_store, &participants, readiness_backend,
              readiness_store, &readiness, "node-1", spatial_config()) ==
                  ParticipantEnrollmentResult::OK &&
              enrollment.activate(registry_backend, registry_store, &participants,
                                  readiness, "node-1") ==
                  ParticipantEnrollmentResult::OK &&
              participants.registry.active_count() == 1,
          "complete readiness must permit a separately persisted activation");

  require(enrollment.reconfigure_active_participant(
              participants, readiness_backend, readiness_store, &readiness,
              "node-1", spatial_config(2, 2)) ==
                  ParticipantEnrollmentResult::OK &&
              participants.registry.find("node-1")->membership ==
                  ParticipantMembership::ACTIVE &&
              readiness.registry.find("node-1")->config.pose_version == 2,
          "active recalibration must update readiness without changing membership");

  readiness_backend.slot_save_succeeds = false;
  require(enrollment.reconfigure_active_participant(
              participants, readiness_backend, readiness_store, &readiness,
              "node-1", spatial_config(3, 3)) ==
                  ParticipantEnrollmentResult::PERSISTENCE_FAILED &&
              participants.registry.find("node-1")->membership ==
                  ParticipantMembership::ACTIVE &&
              readiness.registry.find("node-1")->config.pose_version == 2,
          "failed active recalibration must preserve membership and readiness");
  readiness_backend.slot_save_succeeds = true;

  require(enrollment.forget(registry_backend, registry_store, &participants,
                            readiness_backend, readiness_store, &readiness,
                            "node-1") == ParticipantEnrollmentResult::INVALID_STATE &&
              participants.registry.find("node-1") != nullptr &&
              readiness.registry.find("node-1") != nullptr,
          "forget must reject active participants before removing readiness");

  require(enrollment.configure_participant(
              registry_backend, registry_store, &participants, readiness_backend,
              readiness_store, &readiness, "node-1", spatial_config(3, 3)) ==
                  ParticipantEnrollmentResult::OK &&
              participants.registry.find("node-1")->membership ==
                  ParticipantMembership::QUARANTINED &&
              readiness.registry.find("node-1")->config.pose_version == 3,
          "changing an active profile must first return the participant to quarantine");

  require(enrollment.revoke(registry_backend, registry_store, &participants,
                            "node-1") == ParticipantEnrollmentResult::OK &&
              enrollment.forget(registry_backend, registry_store, &participants,
                                readiness_backend, readiness_store, &readiness,
                                "node-1") == ParticipantEnrollmentResult::OK &&
              participants.registry.find("node-1") == nullptr &&
              readiness.registry.find("node-1") == nullptr,
          "forget must remove registry authority and its readiness profile");
}

void observation_admission_rejects_stale_or_misconfigured_sources() {
  ParticipantRegistry participants = active_registry("node-1");
  ParticipantReadinessRegistry readiness{};
  require(readiness.configure("node-1", spatial_config()) ==
              ParticipantReadinessResult::OK,
          "admission fixture profile must be valid");
  ParticipantObservationMetadata observation{};
  observation.participant_id = "node-1";
  observation.participant_type = ParticipantType::NAMO_NODE;
  observation.site_id = "site-main";
  observation.group_id = "fusion-main";
  observation.boot_id = "boot-a";
  observation.sequence = 1;
  observation.sample_age_ms = 50;
  observation.ttl_ms = 1000;
  observation.pose_version = 1;
  observation.calibration_version = 1;
  observation.track_count = 1;
  observation.presence = true;
  observation.motion = true;
  observation.has_velocity = true;
  observation.has_range = true;

  ObservationAdmission direct_admission{};
  require(direct_admission.admit(
              participants.find("node-1"), readiness.find("node-1"),
              "site-main", observation) == ObservationAdmissionResult::OK,
          "RAM-backed admission fixtures must use the same validation path");

  ObservationAdmission admission{};
  ParticipantRegistry unknown{};
  require(admission.admit(unknown, readiness, "site-main", observation) ==
              ObservationAdmissionResult::UNKNOWN_PARTICIPANT,
          "unknown participants must not enter shadow admission");
  ParticipantRegistry inactive = participants;
  require(inactive.quarantine("node-1") == ParticipantRegistryResult::OK &&
              admission.admit(inactive, readiness, "site-main", observation) ==
                  ObservationAdmissionResult::PARTICIPANT_INACTIVE,
          "quarantined participants must not publish observations");
  require(admission.admit(participants, readiness, "site-other", observation) ==
              ObservationAdmissionResult::SITE_MISMATCH,
          "observations must not cross Site boundaries");
  observation.participant_type = ParticipantType::SATELLITE;
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::TYPE_MISMATCH,
          "observation type must match the approved participant type");
  observation.participant_type = ParticipantType::NAMO_NODE;
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::OK,
          "fresh matching spatial observations must be admitted");
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::DUPLICATE_OR_REORDERED,
          "duplicate observations from the same boot must be rejected");
  observation.boot_id = "boot-b";
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::OK,
          "a new boot identity must reset sequence ordering");
  observation.sequence = 2;
  observation.sample_age_ms = observation.ttl_ms;
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::EXPIRED,
          "observations at or beyond their TTL must be rejected");
  observation.sample_age_ms = 0;
  observation.pose_version = 2;
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::CONFIG_VERSION_MISMATCH,
          "spatial observations must match the assigned pose and calibration versions");
  observation.pose_version = 1;
  observation.group_id = "fusion-other";
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::GROUP_MISMATCH,
          "observations must not cross fusion-group boundaries");
  observation.group_id = "fusion-main";
  observation.sequence = 3;
  observation.has_velocity = false;
  require(admission.admit(participants, readiness, "site-main", observation) ==
              ObservationAdmissionResult::CAPABILITY_VIOLATION,
          "track payload shape must match declared velocity capability");

  observation.has_velocity = true;
  observation.sequence = 4;
  observation.group_id = "fusion-canonical";
  require(admission.admit(participants, readiness, "site-main", observation,
                          "fusion-canonical") ==
              ObservationAdmissionResult::OK,
          "canonical Site assignment must override a stale readiness group");
  observation.sequence = 5;
  observation.group_id = "fusion-main";
  require(admission.admit(participants, readiness, "site-main", observation,
                          "fusion-canonical") ==
              ObservationAdmissionResult::GROUP_MISMATCH,
          "canonical Site assignment must reject the stale readiness group");
}

}  // namespace

void run_participant_readiness_tests() {
  readiness_profiles_distinguish_spatial_and_presence_only_sources();
  spatial_transforms_validate_and_apply_the_approved_correction();
  spatial_readiness_detects_geometry_and_pose_changes();
  readiness_storage_round_trips_and_rolls_back_failed_commits();
  readiness_storage_round_trips_in_external_workspace();
  legacy_v1_spatial_profiles_load_but_require_recalibration();
  legacy_v2_spatial_profiles_load_but_require_recalibration();
  readiness_store_migrates_full_legacy_storage();
  readiness_failed_migration_keeps_legacy_storage_bootable();
  full_readiness_registry_survives_repeated_updates();
  enrollment_requires_readiness_and_quarantines_reconfiguration();
  observation_admission_rejects_stale_or_misconfigured_sources();
}
