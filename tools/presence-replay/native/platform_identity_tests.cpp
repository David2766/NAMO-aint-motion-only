#include "platform_identity.h"
#include "platform_identity_store.h"
#include "site_membership.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using esphome::radar_api_server::PlatformIdentity;
using esphome::radar_api_server::PlatformIdentityCodec;
using esphome::radar_api_server::PlatformIdentityCodecResult;
using esphome::radar_api_server::PlatformIdentityStorageBackend;
using esphome::radar_api_server::PlatformIdentityStorageBlob;
using esphome::radar_api_server::PlatformIdentityStore;
using esphome::radar_api_server::PlatformIdentityStoreResult;
using esphome::radar_api_server::PlatformStateStore;
using esphome::radar_api_server::NodeIdentity;
using esphome::radar_api_server::NodeIdentityCodec;
using esphome::radar_api_server::NodeIdentityStorageBackend;
using esphome::radar_api_server::NodeIdentityStorageBlob;
using esphome::radar_api_server::SiteMembership;
using esphome::radar_api_server::SiteMembershipCommitBlob;
using esphome::radar_api_server::SiteMembershipCodec;
using esphome::radar_api_server::SiteMembershipCodecResult;
using esphome::radar_api_server::SiteMembershipMode;
using esphome::radar_api_server::SiteMembershipStorageBackend;
using esphome::radar_api_server::SiteMembershipStorageBlob;
using esphome::radar_api_server::SiteMembershipStore;
using esphome::radar_api_server::SiteMembershipStoreResult;
using esphome::radar_api_server::build_platform_status_json;
using esphome::radar_api_server::build_platform_descriptor_json;
using esphome::radar_api_server::create_initial_platform_identity;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "platform_identity_tests: %s\n", message);
  std::exit(1);
}

PlatformIdentity make_identity(const char *site_id, const char *node_id) {
  PlatformIdentity identity{};
  std::strncpy(identity.site_id, site_id, PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(identity.node_id, node_id, PlatformIdentity::MAX_ID_LENGTH);
  return identity;
}

std::size_t encode(const PlatformIdentity &identity,
                   std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> *storage) {
  std::size_t size = 0;
  require(PlatformIdentityCodec::encode(identity, storage->data(), storage->size(), &size) ==
              PlatformIdentityCodecResult::OK,
          "valid identity encoding must succeed");
  return size;
}

class FakeIdentityBackend : public PlatformIdentityStorageBackend,
                            public NodeIdentityStorageBackend,
                            public SiteMembershipStorageBackend {
 public:
  bool identity_present{false};
  bool marker_present{false};
  bool save_identity_succeeds{true};
  bool save_marker_succeeds{true};
  int fail_sync_call{0};
  int sync_calls{0};
  PlatformIdentityStorageBlob blob{};
  uint32_t marker{0};
  bool node_present{false};
  bool node_save_succeeds{true};
  NodeIdentityStorageBlob node_blob{};
  bool membership_slot_present[2]{false, false};
  bool membership_slot_save_succeeds{true};
  SiteMembershipStorageBlob membership_slots[2]{};
  bool membership_commit_present{false};
  bool membership_commit_save_succeeds{true};
  SiteMembershipCommitBlob membership_commit{};
  std::vector<std::string> events;

  bool load_identity(PlatformIdentityStorageBlob *output) override {
    if (!this->identity_present)
      return false;
    *output = this->blob;
    return true;
  }
  bool load_marker(uint32_t *output) override {
    if (!this->marker_present)
      return false;
    *output = this->marker;
    return true;
  }
  bool save_identity(const PlatformIdentityStorageBlob &value) override {
    this->events.emplace_back("save_identity");
    if (!this->save_identity_succeeds)
      return false;
    this->blob = value;
    this->identity_present = true;
    return true;
  }
  bool save_marker(uint32_t value) override {
    this->events.emplace_back("save_marker");
    if (!this->save_marker_succeeds)
      return false;
    this->marker = value;
    this->marker_present = true;
    return true;
  }
  bool load_node_identity(NodeIdentityStorageBlob *output) override {
    if (!this->node_present)
      return false;
    *output = this->node_blob;
    return true;
  }
  bool save_node_identity(const NodeIdentityStorageBlob &value) override {
    this->events.emplace_back("save_node");
    if (!this->node_save_succeeds)
      return false;
    this->node_blob = value;
    this->node_present = true;
    return true;
  }
  bool load_site_membership_slot(uint8_t slot,
                                 SiteMembershipStorageBlob *output) override {
    if (slot > 1 || !this->membership_slot_present[slot])
      return false;
    *output = this->membership_slots[slot];
    return true;
  }
  bool save_site_membership_slot(uint8_t slot,
                                 const SiteMembershipStorageBlob &value) override {
    this->events.emplace_back(slot == 0 ? "save_membership_0" : "save_membership_1");
    if (slot > 1 || !this->membership_slot_save_succeeds)
      return false;
    this->membership_slots[slot] = value;
    this->membership_slot_present[slot] = true;
    return true;
  }
  bool load_site_membership_commit(SiteMembershipCommitBlob *output) override {
    if (!this->membership_commit_present)
      return false;
    *output = this->membership_commit;
    return true;
  }
  bool save_site_membership_commit(const SiteMembershipCommitBlob &value) override {
    this->events.emplace_back("save_membership_commit");
    if (!this->membership_commit_save_succeeds)
      return false;
    this->membership_commit = value;
    this->membership_commit_present = true;
    return true;
  }
  bool sync() override {
    this->events.emplace_back("sync");
    this->sync_calls++;
    return this->sync_calls != this->fail_sync_call;
  }
};

void node_and_membership_codecs_reject_corruption() {
  NodeIdentity node{};
  std::strncpy(node.node_id, "namo-node-246f2801a2b3", NodeIdentity::MAX_ID_LENGTH);
  std::array<uint8_t, NodeIdentityCodec::MAX_ENCODED_SIZE> node_storage{};
  std::size_t node_size = 0;
  require(NodeIdentityCodec::encode(node, node_storage.data(), node_storage.size(),
                                    &node_size) == PlatformIdentityCodecResult::OK,
          "a stable node id must encode independently from site membership");
  NodeIdentity decoded_node{};
  require(NodeIdentityCodec::decode(node_storage.data(), node_storage.size(), &decoded_node) ==
              PlatformIdentityCodecResult::OK &&
              std::strcmp(decoded_node.node_id, node.node_id) == 0,
          "node identity must round-trip independently");
  node_storage[9] ^= 0x01;
  require(NodeIdentityCodec::decode(node_storage.data(), node_storage.size(), &decoded_node) ==
              PlatformIdentityCodecResult::CORRUPT,
          "node identity corruption must fail closed");

  SiteMembership membership{};
  std::strncpy(membership.site_id, "site-home", SiteMembership::MAX_ID_LENGTH);
  membership.mode = SiteMembershipMode::STANDALONE;
  membership.generation = 1;
  std::array<uint8_t, SiteMembershipCodec::MAX_ENCODED_SIZE> membership_storage{};
  std::size_t membership_size = 0;
  require(SiteMembershipCodec::encode(membership, membership_storage.data(),
                                      membership_storage.size(), &membership_size) ==
              SiteMembershipCodecResult::OK,
          "a standalone membership must encode independently from node identity");
  SiteMembership decoded_membership{};
  require(SiteMembershipCodec::decode(membership_storage.data(), membership_storage.size(),
                                      &decoded_membership) == SiteMembershipCodecResult::OK &&
              std::strcmp(decoded_membership.site_id, membership.site_id) == 0,
          "site membership must round-trip independently");
  membership_storage[13] ^= 0x01;
  require(SiteMembershipCodec::decode(membership_storage.data(), membership_storage.size(),
                                      &decoded_membership) ==
              SiteMembershipCodecResult::CORRUPT,
          "site membership corruption must fail closed");
}

void site_membership_updates_use_committed_slots() {
  FakeIdentityBackend backend;
  require(SiteMembershipStore::create(backend, "site-home", SiteMembershipMode::STANDALONE) ==
              SiteMembershipStoreResult::CREATED,
          "initial standalone membership must be committed");
  SiteMembership membership{};
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::LOADED &&
              std::strcmp(membership.site_id, "site-home") == 0 &&
              membership.mode == SiteMembershipMode::STANDALONE &&
              membership.generation == 1,
          "the committed standalone membership must load");

  require(SiteMembershipStore::update(backend, "site-building-a",
                                      SiteMembershipMode::ENROLLED) ==
              SiteMembershipStoreResult::UPDATED,
          "site enrollment must commit through the inactive slot");
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::LOADED &&
              std::strcmp(membership.site_id, "site-building-a") == 0 &&
              membership.mode == SiteMembershipMode::ENROLLED &&
              membership.generation == 2,
          "the commit pointer must select the new membership generation");
}

void membership_target_matching_includes_the_mode() {
  SiteMembership membership{};
  std::strcpy(membership.site_id, "site-home");
  membership.mode = SiteMembershipMode::STANDALONE;
  membership.generation = 1;

  require(site_membership_matches_target(
              membership, "site-home", SiteMembershipMode::STANDALONE),
          "an exact membership target must match");
  require(!site_membership_matches_target(
              membership, "site-home", SiteMembershipMode::ENROLLED),
          "same-Site enrollment must still detect a mode transition");
  require(!site_membership_matches_target(
              membership, "site-other", SiteMembershipMode::STANDALONE),
          "a different Site must not match");
}

void failed_membership_updates_keep_a_valid_committed_site() {
  FakeIdentityBackend backend;
  require(SiteMembershipStore::create(backend, "site-home", SiteMembershipMode::STANDALONE) ==
              SiteMembershipStoreResult::CREATED,
          "test membership creation must succeed");

  backend.membership_slot_save_succeeds = false;
  require(SiteMembershipStore::update(backend, "site-rejected", SiteMembershipMode::ENROLLED) ==
              SiteMembershipStoreResult::STORAGE_FAILED,
          "a failed inactive-slot write must abort enrollment");
  SiteMembership membership{};
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::LOADED &&
              std::strcmp(membership.site_id, "site-home") == 0,
          "a failed slot write must leave the previous site active");

  backend.membership_slot_save_succeeds = true;
  backend.membership_commit_save_succeeds = false;
  require(SiteMembershipStore::update(backend, "site-uncommitted",
                                      SiteMembershipMode::ENROLLED) ==
              SiteMembershipStoreResult::STORAGE_FAILED,
          "a failed commit switch must report storage failure");
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::LOADED &&
              std::strcmp(membership.site_id, "site-home") == 0,
          "an uncommitted inactive slot must never replace the active site");
}

void membership_commit_repair_is_unambiguous() {
  FakeIdentityBackend backend;
  backend.membership_commit_save_succeeds = false;
  require(SiteMembershipStore::create(backend, "site-home", SiteMembershipMode::STANDALONE) ==
              SiteMembershipStoreResult::STORAGE_FAILED,
          "an interrupted first membership commit must be reported");
  backend.membership_commit_save_succeeds = true;
  SiteMembership membership{};
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::COMMIT_REPAIRED &&
              std::strcmp(membership.site_id, "site-home") == 0,
          "one valid slot may repair a missing initial commit");

  require(SiteMembershipStore::update(backend, "site-next", SiteMembershipMode::ENROLLED) ==
              SiteMembershipStoreResult::UPDATED,
          "a second committed generation must be writable");
  backend.membership_commit_present = false;
  require(SiteMembershipStore::load(backend, &membership) ==
              SiteMembershipStoreResult::CORRUPT,
          "two valid slots without a commit are ambiguous and must fail closed");
}

void v1_identity_migrates_without_changing_ids() {
  FakeIdentityBackend backend;
  const PlatformIdentity legacy = make_identity("site-home", "namo-node-246f2801a2b3");
  require(PlatformIdentityStore::create(backend, legacy) ==
              PlatformIdentityStoreResult::CREATED,
          "the V1 fixture must be persisted");

  PlatformIdentity migrated{};
  SiteMembership runtime_membership{};
  require(PlatformStateStore::load_or_migrate(backend, backend, backend, &migrated,
                                               &runtime_membership) ==
              PlatformIdentityStoreResult::MIGRATED,
          "a valid V1 identity must migrate to split V2 storage");
  require(std::strcmp(migrated.node_id, legacy.node_id) == 0 &&
              std::strcmp(migrated.site_id, legacy.site_id) == 0,
          "migration must preserve both public ids exactly");
  require(std::strcmp(runtime_membership.site_id, legacy.site_id) == 0 &&
              runtime_membership.mode == SiteMembershipMode::STANDALONE &&
              runtime_membership.generation == 1,
          "migration must expose the active membership to the runtime");

  PlatformIdentity rebooted{};
  require(PlatformStateStore::load_or_migrate(backend, backend, backend, &rebooted) ==
              PlatformIdentityStoreResult::LOADED &&
              std::strcmp(rebooted.node_id, legacy.node_id) == 0 &&
              std::strcmp(rebooted.site_id, legacy.site_id) == 0,
          "later boots must load V2 without remigrating or changing ids");
}

void interrupted_v2_migration_resumes_from_v1() {
  FakeIdentityBackend backend;
  const PlatformIdentity legacy = make_identity("site-home", "namo-node-246f2801a2b3");
  require(PlatformIdentityStore::create(backend, legacy) ==
              PlatformIdentityStoreResult::CREATED,
          "the V1 recovery journal must exist");
  backend.membership_commit_save_succeeds = false;
  PlatformIdentity output{};
  require(PlatformStateStore::load_or_migrate(backend, backend, backend, &output) ==
              PlatformIdentityStoreResult::STORAGE_FAILED,
          "an interrupted membership commit must stop migration");

  backend.membership_commit_save_succeeds = true;
  require(PlatformStateStore::load_or_migrate(backend, backend, backend, &output) ==
              PlatformIdentityStoreResult::MARKER_REPAIRED &&
              std::strcmp(output.node_id, legacy.node_id) == 0 &&
              std::strcmp(output.site_id, legacy.site_id) == 0,
          "the next boot must finish an unambiguous V2 migration without new ids");
}

void site_changes_never_rewrite_node_identity() {
  FakeIdentityBackend backend;
  const PlatformIdentity created = make_identity("site-home", "namo-node-246f2801a2b3");
  require(PlatformStateStore::create(backend, backend, backend, created) ==
              PlatformIdentityStoreResult::CREATED,
          "new platform state must create its recovery journal and V2 records");
  const auto original_node_blob = backend.node_blob.bytes;
  require(SiteMembershipStore::update(backend, "site-building-a",
                                      SiteMembershipMode::ENROLLED) ==
              SiteMembershipStoreResult::UPDATED,
          "the mutable site membership must update independently");

  PlatformIdentity loaded{};
  require(PlatformStateStore::load_or_migrate(backend, backend, backend, &loaded) ==
              PlatformIdentityStoreResult::LOADED &&
              std::strcmp(loaded.node_id, created.node_id) == 0 &&
              std::strcmp(loaded.site_id, "site-building-a") == 0,
          "site adoption must preserve the immutable node id");
  require(backend.node_blob.bytes == original_node_blob,
          "site adoption must not rewrite node identity storage");
}

void identity_survives_repeated_loads() {
  const PlatformIdentity source = make_identity("site-home", "namo-bedroom-west");
  std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> storage{};
  const std::size_t encoded_size = encode(source, &storage);

  PlatformIdentity first_boot{};
  PlatformIdentity second_boot{};
  require(PlatformIdentityCodec::decode(storage.data(), storage.size(), &first_boot) ==
              PlatformIdentityCodecResult::OK,
          "first reboot load must succeed");
  require(PlatformIdentityCodec::decode(storage.data(), storage.size(), &second_boot) ==
              PlatformIdentityCodecResult::OK,
          "later reboot load must succeed");
  require(std::strcmp(first_boot.site_id, source.site_id) == 0 &&
              std::strcmp(first_boot.node_id, source.node_id) == 0 &&
              std::strcmp(second_boot.site_id, source.site_id) == 0 &&
              std::strcmp(second_boot.node_id, source.node_id) == 0,
          "stable identity must not change across boots");
  require(encoded_size <= storage.size(), "the V1 record must stay within its fixed storage bound");
}

void erased_and_legacy_storage_are_not_silent_identities() {
  std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> erased{};
  erased.fill(0xFF);
  PlatformIdentity output = make_identity("unchanged-site", "unchanged-node");
  require(PlatformIdentityCodec::decode(erased.data(), erased.size(), &output) ==
              PlatformIdentityCodecResult::EMPTY,
          "erased pre-platform storage must be reported as empty");
  require(std::strcmp(output.site_id, "unchanged-site") == 0,
          "failed loads must not partially replace the current identity");

  const char legacy_name[] = "presence-sensor-a1b2c3";
  require(PlatformIdentityCodec::decode(reinterpret_cast<const uint8_t *>(legacy_name),
                                        sizeof(legacy_name), &output) ==
              PlatformIdentityCodecResult::CORRUPT,
          "legacy hostnames must not be mistaken for stable platform identity");
}

void invalid_ids_and_small_buffers_are_rejected() {
  std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> storage{};
  std::size_t size = 0;
  require(PlatformIdentityCodec::encode(make_identity("-bad", "node-1"), storage.data(),
                                        storage.size(), &size) ==
              PlatformIdentityCodecResult::INVALID_ID,
          "invalid site ids must be rejected");
  require(PlatformIdentityCodec::encode(make_identity("site-1", "node 1"), storage.data(),
                                        storage.size(), &size) ==
              PlatformIdentityCodecResult::INVALID_ID,
          "invalid node ids must be rejected");

  const std::string maximum_site(64, 's');
  const std::string maximum_node(64, 'n');
  const PlatformIdentity maximum = make_identity(maximum_site.c_str(), maximum_node.c_str());
  require(PlatformIdentityCodec::encode(maximum, storage.data(), storage.size(), &size) ==
              PlatformIdentityCodecResult::OK,
          "maximum length ids must fit the fixed storage budget");
  require(size <= PlatformIdentityCodec::MAX_ENCODED_SIZE,
          "encoded identity must remain inside its declared bound");
  require(PlatformIdentityCodec::encode(maximum, storage.data(), size - 1, &size) ==
              PlatformIdentityCodecResult::INVALID_ARGUMENT,
          "undersized storage must fail without truncating identity");
}

void corruption_and_unknown_versions_fail_closed() {
  const PlatformIdentity source = make_identity("site-home", "node-west");
  std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> storage{};
  const std::size_t encoded_size = encode(source, &storage);
  PlatformIdentity output{};

  auto corrupted = storage;
  corrupted[12] ^= 0x20;
  require(PlatformIdentityCodec::decode(corrupted.data(), corrupted.size(), &output) ==
              PlatformIdentityCodecResult::CORRUPT,
          "payload corruption must be caught by the checksum");

  auto unsupported = storage;
  unsupported[4] = 2;
  unsupported[5] = 0;
  require(PlatformIdentityCodec::decode(unsupported.data(), unsupported.size(), &output) ==
              PlatformIdentityCodecResult::UNSUPPORTED_VERSION,
          "future storage versions must not be guessed");
  require(PlatformIdentityCodec::decode(storage.data(), encoded_size - 1, &output) ==
              PlatformIdentityCodecResult::CORRUPT,
          "torn or truncated writes must be rejected");

  auto dirty_padding = storage;
  dirty_padding[encoded_size] = 0;
  require(PlatformIdentityCodec::decode(dirty_padding.data(), dirty_padding.size(), &output) ==
              PlatformIdentityCodecResult::CORRUPT,
          "unexpected trailing data must not be accepted as a valid record");
}

void initial_identity_uses_stable_mac_and_random_site_seed() {
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x01, 0xA2, 0xB3};
  const uint8_t random[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                              0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  PlatformIdentity identity{};
  require(create_initial_platform_identity(mac, random, &identity) ==
              PlatformIdentityCodecResult::OK,
          "valid hardware seeds must create an identity");
  require(std::strcmp(identity.node_id, "namo-node-246f2801a2b3") == 0,
          "node identity must use the complete stable hardware MAC");
  require(std::strcmp(identity.site_id,
                      "site-00112233445566778899aabbccddeeff") == 0,
          "site identity must use the full random seed");

  const uint8_t invalid_mac[6]{};
  require(create_initial_platform_identity(invalid_mac, random, &identity) ==
              PlatformIdentityCodecResult::INVALID_SEED,
          "an invalid hardware identity must not create a node id");
}

void store_creation_orders_identity_before_marker() {
  FakeIdentityBackend backend;
  PlatformIdentity loaded{};
  require(PlatformIdentityStore::load(backend, &loaded) ==
              PlatformIdentityStoreResult::EMPTY,
          "a device predating platform identity must start empty");

  const PlatformIdentity identity = make_identity("site-home", "node-west");
  require(PlatformIdentityStore::create(backend, identity) ==
              PlatformIdentityStoreResult::CREATED,
          "first identity creation must persist successfully");
  require(backend.events ==
              std::vector<std::string>{"save_identity", "sync", "save_marker", "sync"},
          "identity must be committed before its initialized marker");

  require(PlatformIdentityStore::load(backend, &loaded) ==
              PlatformIdentityStoreResult::LOADED,
          "a persisted identity must load on reboot");
  require(std::strcmp(loaded.site_id, identity.site_id) == 0 &&
              std::strcmp(loaded.node_id, identity.node_id) == 0,
          "preference round-trip must preserve both stable ids");
}

void store_repairs_only_a_valid_identity_without_marker() {
  FakeIdentityBackend backend;
  const PlatformIdentity identity = make_identity("site-home", "node-west");
  require(PlatformIdentityStore::create(backend, identity) ==
              PlatformIdentityStoreResult::CREATED,
          "test identity creation must succeed");
  backend.marker_present = false;
  backend.events.clear();
  backend.sync_calls = 0;

  PlatformIdentity loaded{};
  require(PlatformIdentityStore::load(backend, &loaded) ==
              PlatformIdentityStoreResult::MARKER_REPAIRED,
          "a committed identity may repair a missing trailing marker");
  require(backend.events == std::vector<std::string>{"save_marker", "sync"},
          "marker repair must not rewrite the stable identity");

  backend.identity_present = false;
  require(PlatformIdentityStore::load(backend, &loaded) ==
              PlatformIdentityStoreResult::CORRUPT,
          "a marker without its identity must fail closed");
}

void store_does_not_overwrite_corruption_or_failed_writes() {
  FakeIdentityBackend corrupt_backend;
  const PlatformIdentity identity = make_identity("site-home", "node-west");
  require(PlatformIdentityStore::create(corrupt_backend, identity) ==
              PlatformIdentityStoreResult::CREATED,
          "test identity creation must succeed");
  corrupt_backend.blob.bytes[12] ^= 0x01;
  corrupt_backend.events.clear();
  PlatformIdentity loaded = make_identity("unchanged-site", "unchanged-node");
  require(PlatformIdentityStore::load(corrupt_backend, &loaded) ==
              PlatformIdentityStoreResult::CORRUPT,
          "corrupt persisted identity must not be regenerated");
  require(corrupt_backend.events.empty() &&
              std::strcmp(loaded.site_id, "unchanged-site") == 0,
          "corruption handling must not write or partially replace identity");

  FakeIdentityBackend first_sync_failure;
  first_sync_failure.fail_sync_call = 1;
  require(PlatformIdentityStore::create(first_sync_failure, identity) ==
              PlatformIdentityStoreResult::STORAGE_FAILED,
          "identity commit failure must stop initialization");
  require(first_sync_failure.events ==
              std::vector<std::string>{"save_identity", "sync"},
          "the marker must never be attempted before identity commit succeeds");

  FakeIdentityBackend interrupted_backend;
  interrupted_backend.save_marker_succeeds = false;
  require(PlatformIdentityStore::create(interrupted_backend, identity) ==
              PlatformIdentityStoreResult::STORAGE_FAILED,
          "a marker write failure must be reported");
  interrupted_backend.save_marker_succeeds = true;
  interrupted_backend.events.clear();
  interrupted_backend.sync_calls = 0;
  require(PlatformIdentityStore::load(interrupted_backend, &loaded) ==
              PlatformIdentityStoreResult::MARKER_REPAIRED,
          "the next boot must recover after identity committed before marker failure");

  interrupted_backend.marker = 0xDEADBEEFU;
  require(PlatformIdentityStore::load(interrupted_backend, &loaded) ==
              PlatformIdentityStoreResult::CORRUPT,
          "an unexpected initialization marker must fail closed");
}

void platform_status_is_bounded_and_does_not_claim_unimplemented_features() {
  const PlatformIdentity identity = make_identity("site-home", "namo-node-246f2801a2b3");
  const std::string ready =
      build_platform_status_json(identity, true, true, 2, 2, 1);
  require(ready ==
              "{\"schemaVersion\":1,\"identityReady\":true,\"siteId\":\"site-home\","
              "\"nodeId\":\"namo-node-246f2801a2b3\",\"mode\":\"standalone\","
              "\"maxParticipants\":20,\"registryReady\":true,"
              "\"remoteParticipantCount\":2,\"connectedRemoteParticipantCount\":2,"
              "\"activeRemoteParticipantCount\":1,"
              "\"readinessReady\":false,\"configuredRemoteParticipantCount\":0,"
              "\"fusionEnabled\":false,\"pairingSupported\":true,\"pairingEnabled\":false}",
          "ready platform status must expose only the stable public identity contract");

  const std::string unavailable =
      build_platform_status_json(identity, false, false, 2, 2, 1);
  require(unavailable ==
              "{\"schemaVersion\":1,\"identityReady\":false,\"mode\":\"standalone\","
              "\"maxParticipants\":20,\"registryReady\":false,"
              "\"remoteParticipantCount\":0,\"connectedRemoteParticipantCount\":0,"
              "\"activeRemoteParticipantCount\":0,"
              "\"readinessReady\":false,\"configuredRemoteParticipantCount\":0,"
              "\"fusionEnabled\":false,\"pairingSupported\":true,\"pairingEnabled\":false}",
          "unavailable platform status must omit identity values");
  require(unavailable.find("site-home") == std::string::npos &&
              unavailable.find("namo-node") == std::string::npos &&
              ready.find("private") == std::string::npos &&
              ready.find("certificate") == std::string::npos,
          "platform status must not leak unavailable identity or future credentials");

  const std::string degraded =
      build_platform_status_json(identity, true, false, 19, 19, 19);
  require(degraded ==
              "{\"schemaVersion\":1,\"identityReady\":true,\"siteId\":\"site-home\","
              "\"nodeId\":\"namo-node-246f2801a2b3\",\"mode\":\"standalone\","
              "\"maxParticipants\":20,\"registryReady\":false,"
              "\"remoteParticipantCount\":0,\"connectedRemoteParticipantCount\":0,"
              "\"activeRemoteParticipantCount\":0,"
              "\"readinessReady\":false,\"configuredRemoteParticipantCount\":0,"
              "\"fusionEnabled\":false,\"pairingSupported\":true,\"pairingEnabled\":false}",
          "registry failure must preserve local identity while hiding unusable peer counts");

  const std::string pairing =
      build_platform_status_json(identity, true, true, 2, 2, 1, true);
  require(pairing.find("\"fusionEnabled\":false,\"pairingSupported\":true,"
                       "\"pairingEnabled\":true") != std::string::npos,
          "an open enrollment window must be visible without enabling fusion");

  const std::string configured =
      build_platform_status_json(identity, true, true, 2, 2, 1, false, true, 2);
  require(configured.find("\"readinessReady\":true,"
                          "\"configuredRemoteParticipantCount\":2") !=
              std::string::npos,
          "platform status must expose readiness persistence without enabling fusion");

  const std::string enrolled = build_platform_status_json(
      identity, true, true, 1, 1, 0, false, true, 1,
      SiteMembershipMode::ENROLLED);
  require(enrolled.find("\"mode\":\"enrolled\"") != std::string::npos,
          "an enrolled joiner must expose its persisted membership mode");

  const std::string production = build_platform_status_json(
      identity, true, true, 1, 1, 1, false, true, 1,
      SiteMembershipMode::ENROLLED, true);
  require(production.find("\"fusionEnabled\":true") != std::string::npos,
          "a production build must expose its compiled fusion capability");
}

void platform_descriptor_is_bounded_and_matches_v1_capabilities() {
  const PlatformIdentity identity = make_identity("site-home", "namo-node-246f2801a2b3");
  const std::string descriptor =
      build_platform_descriptor_json(identity, true, "0.5.5-d");
  require(descriptor ==
              "{\"protocolVersion\":1,\"participantId\":\"namo-node-246f2801a2b3\","
              "\"participantType\":\"namo_node\",\"product\":\"namo-spatial-sensor\","
              "\"softwareVersion\":\"0.5.5-d\",\"capabilities\":{"
              "\"pointTracks\":true,\"velocity\":true,\"positionUncertainty\":true,"
              "\"range\":true,\"presence\":true,\"motion\":true,"
              "\"zonePresence\":false,\"bridge\":false,\"maxTargets\":3},"
              "\"protocolRange\":{\"minimum\":1,\"maximum\":1}}",
          "the descriptor must exactly match the implemented V1 discovery contract");
  require(descriptor.size() < 768,
          "the descriptor must remain within its fixed serialization bound");
  require(descriptor.find("site-home") == std::string::npos &&
              descriptor.find("private") == std::string::npos &&
              descriptor.find("certificate") == std::string::npos &&
              descriptor.find("attestation") == std::string::npos,
          "discovery must not expose Site membership or future credentials");

  require(build_platform_descriptor_json(identity, false, "0.5.5-d").empty(),
          "an unavailable identity must not produce a fake descriptor");
  require(build_platform_descriptor_json(identity, true, nullptr).empty() &&
              build_platform_descriptor_json(identity, true, "").empty(),
          "a descriptor without a software version must fail closed");
}

}  // namespace

void run_platform_identity_tests() {
  node_and_membership_codecs_reject_corruption();
  site_membership_updates_use_committed_slots();
  membership_target_matching_includes_the_mode();
  failed_membership_updates_keep_a_valid_committed_site();
  membership_commit_repair_is_unambiguous();
  v1_identity_migrates_without_changing_ids();
  interrupted_v2_migration_resumes_from_v1();
  site_changes_never_rewrite_node_identity();
  identity_survives_repeated_loads();
  erased_and_legacy_storage_are_not_silent_identities();
  invalid_ids_and_small_buffers_are_rejected();
  corruption_and_unknown_versions_fail_closed();
  initial_identity_uses_stable_mac_and_random_site_seed();
  store_creation_orders_identity_before_marker();
  store_repairs_only_a_valid_identity_without_marker();
  store_does_not_overwrite_corruption_or_failed_writes();
  platform_status_is_bounded_and_does_not_claim_unimplemented_features();
  platform_descriptor_is_bounded_and_matches_v1_capabilities();
}
