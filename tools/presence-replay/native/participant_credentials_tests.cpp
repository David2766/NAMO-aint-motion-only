#include "participant_credentials.h"
#include "radar_storage.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::radar_api_server;

namespace {

class FakeCredentialBackend : public ParticipantCredentialStorageBackend {
 public:
  bool load_participant_credential_slot(
      uint8_t slot, ParticipantCredentialStorageBlob *blob) override {
    if (slot > 1 || !slot_present[slot] || blob == nullptr)
      return false;
    *blob = slots[slot];
    return true;
  }

  bool save_participant_credential_slot(
      uint8_t slot, const ParticipantCredentialStorageBlob &blob) override {
    events.push_back("save_slot_" + std::to_string(slot));
    if (slot > 1 || !slot_save_succeeds)
      return false;
    slots[slot] = blob;
    slot_present[slot] = true;
    return true;
  }

  bool load_participant_credential_commit(
      ParticipantCredentialCommitBlob *blob) override {
    if (!commit_present || blob == nullptr)
      return false;
    *blob = commit;
    return true;
  }

  bool save_participant_credential_commit(
      const ParticipantCredentialCommitBlob &blob) override {
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

  ParticipantCredentialStorageBlob slots[2]{};
  ParticipantCredentialCommitBlob commit{};
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
  std::fprintf(stderr, "participant_credentials_tests: %s\n", message);
  std::exit(1);
}

std::array<uint8_t, 32> key_with_seed(uint8_t seed) {
  std::array<uint8_t, 32> key{};
  for (std::size_t index = 0; index < key.size(); ++index)
    key[index] = static_cast<uint8_t>(seed + index);
  return key;
}

void codec_round_trip_preserves_secret_state() {
  ParticipantCredentialSnapshot input{};
  input.generation = 7;
  const auto first_key = key_with_seed(10);
  const auto second_key = key_with_seed(80);
  require(input.registry.set("node-a", 101, 2, first_key.data()) ==
              ParticipantCredentialResult::OK,
          "first credential fixture should be valid");
  require(input.registry.set("satellite-b", 202, 4, second_key.data(), false) ==
              ParticipantCredentialResult::OK,
          "disabled credential fixture should be valid");

  std::array<uint8_t, ParticipantCredentialCodec::MAX_ENCODED_SIZE> encoded{};
  std::size_t encoded_size = 0;
  require(ParticipantCredentialCodec::encode(
              input, encoded.data(), encoded.size(), &encoded_size) ==
              ParticipantCredentialCodecResult::OK,
          "credential snapshot should encode");
  ParticipantCredentialSnapshot decoded{};
  require(ParticipantCredentialCodec::decode(encoded.data(), encoded_size,
                                              &decoded) ==
              ParticipantCredentialCodecResult::OK,
          "credential snapshot should decode");
  const auto *first = decoded.registry.find("node-a");
  const auto *second = decoded.registry.find("satellite-b");
  require(decoded.generation == 7 && first != nullptr && second != nullptr &&
              first->enabled && !second->enabled && first->key == first_key &&
              second->key == second_key,
          "round trip must preserve key versions, keys, and enabled state");

  encoded[encoded_size - 1] ^= 0x80;
  require(ParticipantCredentialCodec::decode(encoded.data(), encoded_size,
                                              &decoded) ==
              ParticipantCredentialCodecResult::CORRUPT,
          "credential corruption must fail closed");
}

void provisioned_resolution_does_not_open_observation_authority() {
  ParticipantCredentialRegistry registry;
  const auto key = key_with_seed(42);
  require(registry.set("node-quarantined", 303, 1, key.data(), false) ==
              ParticipantCredentialResult::OK,
          "disabled credential fixture should be valid");
  require(registry.resolve(303, 1) == nullptr,
          "disabled credentials must not authorize observation traffic");
  const auto *control = registry.resolve_provisioned(303, 1);
  require(control != nullptr && !control->enabled &&
              std::strcmp(control->participant_id, "node-quarantined") == 0,
          "provisioned credentials must authenticate control traffic");
  require(registry.resolve_provisioned(303, 2) == nullptr,
          "control lookup must still enforce key version");
  require(registry.set_enabled("node-quarantined", true) ==
              ParticipantCredentialResult::OK &&
              registry.resolve(303, 1) != nullptr,
          "activation should open the existing observation lookup");
}

void store_commit_failure_keeps_previous_generation() {
  FakeCredentialBackend backend;
  ParticipantCredentialStore store;
  ParticipantCredentialSnapshot snapshot{};
  ParticipantCredentialRegistry registry;
  const auto first_key = key_with_seed(1);
  require(registry.set("node-a", 11, 1, first_key.data()) ==
              ParticipantCredentialResult::OK,
          "initial store fixture should be valid");
  require(store.create(backend, registry, &snapshot) ==
              ParticipantCredentialStoreResult::CREATED,
          "credential store should create transactionally");
  require(backend.events == std::vector<std::string>{
                                "save_slot_0", "sync", "save_commit", "sync"},
          "slot must be durable before its commit marker");

  const auto second_key = key_with_seed(33);
  require(registry.set("node-b", 22, 1, second_key.data()) ==
              ParticipantCredentialResult::OK,
          "updated store fixture should be valid");
  backend.commit_save_succeeds = false;
  require(store.update(backend, registry, &snapshot) ==
              ParticipantCredentialStoreResult::STORAGE_FAILED,
          "failed commit marker must fail the update");
  require(snapshot.generation == 1 && snapshot.registry.find("node-b") == nullptr,
          "failed persistence must not publish a new runtime snapshot");

  backend.commit_save_succeeds = true;
  ParticipantCredentialStore rebooted_store;
  ParticipantCredentialSnapshot rebooted{};
  require(rebooted_store.load(backend, &rebooted) ==
              ParticipantCredentialStoreResult::LOADED &&
              rebooted.generation == 1 &&
              rebooted.registry.find("node-a") != nullptr &&
              rebooted.registry.find("node-b") == nullptr,
          "reboot after commit failure must load the previous generation");
}

void corrupt_committed_slot_repairs_to_previous_copy() {
  FakeCredentialBackend backend;
  ParticipantCredentialStore store;
  ParticipantCredentialSnapshot snapshot{};
  ParticipantCredentialRegistry registry;
  const auto key = key_with_seed(7);
  require(registry.set("node-a", 31, 1, key.data()) ==
              ParticipantCredentialResult::OK &&
              store.create(backend, registry, &snapshot) ==
                  ParticipantCredentialStoreResult::CREATED,
          "repair fixture should create");
  require(registry.set("node-b", 32, 1, key.data()) ==
              ParticipantCredentialResult::OK &&
              store.update(backend, registry, &snapshot) ==
                  ParticipantCredentialStoreResult::UPDATED,
          "repair fixture should update into the alternate slot");
  backend.slots[1].bytes[0] ^= 0x01;

  ParticipantCredentialStore rebooted_store;
  ParticipantCredentialSnapshot rebooted{};
  require(rebooted_store.load(backend, &rebooted) ==
              ParticipantCredentialStoreResult::COMMIT_REPAIRED &&
              rebooted.generation == 1 &&
              rebooted.registry.find("node-a") != nullptr &&
              rebooted.registry.find("node-b") == nullptr,
          "a corrupt committed slot must fall back to the previous valid copy");
}

void platform_storage_growth_preserves_shipped_offsets() {
  constexpr uint32_t partition_size = 0x1F0000;
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_0) ==
              partition_size - 6 * 4096,
          "registry slot zero must keep its shipped absolute offset");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::PARTICIPANT_READINESS_COMMIT) ==
              partition_size - 4096,
          "readiness commit must remain the final partition sector");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::PARTICIPANT_CREDENTIAL_SLOT_0) ==
              partition_size - 9 * 4096,
          "credential storage must grow toward lower addresses");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::PARTICIPANT_CREDENTIAL_COMMIT) ==
              partition_size - 7 * 4096,
          "credential commit must end immediately before legacy state");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::SITE_CONFIGURATION_SLOT_0) ==
              partition_size - 12 * 4096,
          "Site configuration must grow below every shipped platform sector");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size,
              RadarPlatformBlob::SITE_CONFIGURATION_COMMIT) ==
              partition_size - 10 * 4096,
          "Site configuration commit must end before credential state");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size, RadarPlatformBlob::SITE_DIRECTORY_SLOT_0) ==
              partition_size - 15 * 4096,
          "Site directory must grow into the final reserved lower sectors");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size, RadarPlatformBlob::SITE_DIRECTORY_COMMIT) ==
              partition_size - 13 * 4096,
          "Site directory commit must end before Site configuration state");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size, RadarPlatformBlob::GROUP_ROSTER_SLOT_0) ==
              RadarStorage::GROUP_ROSTER_STORAGE_OFFSET,
          "Group roster slot zero must use the reserved statistics tail");
  require(RadarStorage::platform_blob_offset_for_partition(
              partition_size, RadarPlatformBlob::GROUP_ROSTER_COMMIT) ==
              RadarStorage::GROUP_ROSTER_STORAGE_OFFSET + 2 * 4096,
          "Group roster commit must remain inside the reserved statistics tail");
  require(RadarStorage::GROUP_ROSTER_STORAGE_OFFSET + 3 * 4096 ==
              0x81000,
          "Group roster storage must end exactly before legacy image slot one");
  require(RadarStorage::PLATFORM_SECTOR_COUNT == 18,
          "platform storage must expose exactly eighteen logical blobs");
  require(RadarStorage::TAIL_PLATFORM_SECTOR_COUNT == 15,
          "previously shipped platform state must retain fifteen tail sectors");
}

void platform_storage_io_is_bounded_to_internal_staging() {
  require(RadarStorage::platform_io_chunk_size(0) == 0 &&
              RadarStorage::platform_io_chunk_size(1) == 1 &&
              RadarStorage::platform_io_chunk_size(255) == 255 &&
              RadarStorage::platform_io_chunk_size(256) == 256 &&
              RadarStorage::platform_io_chunk_size(257) == 256,
          "platform storage must cap each flash transfer to one staging chunk");

  for (const uint32_t size : {1u, 255u, 256u, 257u, 4095u, 4096u}) {
    uint32_t cursor = 0;
    uint32_t chunks = 0;
    while (cursor < size) {
      const uint32_t chunk =
          RadarStorage::platform_io_chunk_size(size - cursor);
      require(chunk > 0 &&
                  chunk <= RadarStorage::PLATFORM_IO_CHUNK_SIZE &&
                  chunk <= size - cursor,
              "platform storage chunk must remain bounded by the payload");
      cursor += chunk;
      chunks++;
    }
    require(cursor == size,
            "platform storage chunks must cover the complete payload");
    require(chunks ==
                (size + RadarStorage::PLATFORM_IO_CHUNK_SIZE - 1) /
                    RadarStorage::PLATFORM_IO_CHUNK_SIZE,
            "platform storage chunk count must be deterministic");
  }
}

}  // namespace

void run_participant_credentials_tests() {
  codec_round_trip_preserves_secret_state();
  provisioned_resolution_does_not_open_observation_authority();
  store_commit_failure_keeps_previous_generation();
  corrupt_committed_slot_repairs_to_previous_copy();
  platform_storage_growth_preserves_shipped_offsets();
  platform_storage_io_is_bounded_to_internal_staging();
}
