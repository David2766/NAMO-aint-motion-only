#include "platform_identity_store.h"

#include "participant_registry.h"

#include <cstdio>
#include <cstring>

#ifdef USE_ESP32
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#endif

namespace esphome {
namespace radar_api_server {

PlatformIdentityStoreResult PlatformIdentityStore::load(
    PlatformIdentityStorageBackend &backend, PlatformIdentity *identity) {
  if (identity == nullptr)
    return PlatformIdentityStoreResult::STORAGE_FAILED;

  PlatformIdentityStorageBlob blob{};
  uint32_t marker = 0;
  const bool identity_loaded = backend.load_identity(&blob);
  const bool marker_loaded = backend.load_marker(&marker);

  if (!identity_loaded)
    return marker_loaded ? PlatformIdentityStoreResult::CORRUPT
                         : PlatformIdentityStoreResult::EMPTY;

  PlatformIdentity decoded{};
  const auto decode_result = PlatformIdentityCodec::decode(
      blob.bytes.data(), blob.bytes.size(), &decoded);
  if (decode_result == PlatformIdentityCodecResult::UNSUPPORTED_VERSION)
    return PlatformIdentityStoreResult::UNSUPPORTED_VERSION;
  if (decode_result != PlatformIdentityCodecResult::OK)
    return PlatformIdentityStoreResult::CORRUPT;
  if (marker_loaded && marker != INITIALIZED_MARKER)
    return PlatformIdentityStoreResult::CORRUPT;

  PlatformIdentityStoreResult result = PlatformIdentityStoreResult::LOADED;
  if (!marker_loaded) {
    if (!backend.save_marker(INITIALIZED_MARKER) || !backend.sync())
      return PlatformIdentityStoreResult::STORAGE_FAILED;
    result = PlatformIdentityStoreResult::MARKER_REPAIRED;
  }

  *identity = decoded;
  return result;
}

PlatformIdentityStoreResult PlatformIdentityStore::create(
    PlatformIdentityStorageBackend &backend, const PlatformIdentity &identity) {
  PlatformIdentityStorageBlob blob{};
  std::size_t encoded_size = 0;
  if (PlatformIdentityCodec::encode(identity, blob.bytes.data(), blob.bytes.size(),
                                    &encoded_size) != PlatformIdentityCodecResult::OK ||
      encoded_size == 0)
    return PlatformIdentityStoreResult::GENERATION_FAILED;

  if (!backend.save_identity(blob) || !backend.sync())
    return PlatformIdentityStoreResult::STORAGE_FAILED;
  if (!backend.save_marker(INITIALIZED_MARKER) || !backend.sync())
    return PlatformIdentityStoreResult::STORAGE_FAILED;
  return PlatformIdentityStoreResult::CREATED;
}

NodeIdentityStoreResult NodeIdentityStore::load(NodeIdentityStorageBackend &backend,
                                                NodeIdentity *identity) {
  if (identity == nullptr)
    return NodeIdentityStoreResult::STORAGE_FAILED;
  NodeIdentityStorageBlob blob{};
  if (!backend.load_node_identity(&blob))
    return NodeIdentityStoreResult::EMPTY;
  NodeIdentity decoded{};
  const auto result = NodeIdentityCodec::decode(blob.bytes.data(), blob.bytes.size(), &decoded);
  if (result == PlatformIdentityCodecResult::EMPTY)
    return NodeIdentityStoreResult::EMPTY;
  if (result == PlatformIdentityCodecResult::UNSUPPORTED_VERSION)
    return NodeIdentityStoreResult::UNSUPPORTED_VERSION;
  if (result != PlatformIdentityCodecResult::OK)
    return NodeIdentityStoreResult::CORRUPT;
  *identity = decoded;
  return NodeIdentityStoreResult::LOADED;
}

NodeIdentityStoreResult NodeIdentityStore::create(NodeIdentityStorageBackend &backend,
                                                  const NodeIdentity &identity) {
  NodeIdentityStorageBlob blob{};
  std::size_t encoded_size = 0;
  if (NodeIdentityCodec::encode(identity, blob.bytes.data(), blob.bytes.size(),
                                &encoded_size) != PlatformIdentityCodecResult::OK ||
      encoded_size == 0)
    return NodeIdentityStoreResult::GENERATION_FAILED;
  if (!backend.save_node_identity(blob) || !backend.sync())
    return NodeIdentityStoreResult::STORAGE_FAILED;
  return NodeIdentityStoreResult::CREATED;
}

namespace {

bool legacy_result_is_ready(PlatformIdentityStoreResult result) {
  return result == PlatformIdentityStoreResult::LOADED ||
         result == PlatformIdentityStoreResult::CREATED ||
         result == PlatformIdentityStoreResult::MIGRATED ||
         result == PlatformIdentityStoreResult::MARKER_REPAIRED;
}

PlatformIdentityStoreResult map_node_failure(NodeIdentityStoreResult result) {
  if (result == NodeIdentityStoreResult::UNSUPPORTED_VERSION)
    return PlatformIdentityStoreResult::UNSUPPORTED_VERSION;
  if (result == NodeIdentityStoreResult::STORAGE_FAILED)
    return PlatformIdentityStoreResult::STORAGE_FAILED;
  if (result == NodeIdentityStoreResult::GENERATION_FAILED)
    return PlatformIdentityStoreResult::GENERATION_FAILED;
  return PlatformIdentityStoreResult::CORRUPT;
}

PlatformIdentityStoreResult map_membership_failure(SiteMembershipStoreResult result) {
  if (result == SiteMembershipStoreResult::UNSUPPORTED_VERSION)
    return PlatformIdentityStoreResult::UNSUPPORTED_VERSION;
  if (result == SiteMembershipStoreResult::STORAGE_FAILED)
    return PlatformIdentityStoreResult::STORAGE_FAILED;
  if (result == SiteMembershipStoreResult::INVALID_MEMBERSHIP)
    return PlatformIdentityStoreResult::GENERATION_FAILED;
  return PlatformIdentityStoreResult::CORRUPT;
}

void compose_platform_identity(const NodeIdentity &node,
                               const SiteMembership &membership,
                               PlatformIdentity *identity) {
  PlatformIdentity composed{};
  std::strncpy(composed.node_id, node.node_id, PlatformIdentity::MAX_ID_LENGTH);
  std::strncpy(composed.site_id, membership.site_id, PlatformIdentity::MAX_ID_LENGTH);
  composed.node_id[PlatformIdentity::MAX_ID_LENGTH] = '\0';
  composed.site_id[PlatformIdentity::MAX_ID_LENGTH] = '\0';
  *identity = composed;
}

}  // namespace

PlatformIdentityStoreResult PlatformStateStore::load_or_migrate(
    PlatformIdentityStorageBackend &legacy_backend,
    NodeIdentityStorageBackend &node_backend,
    SiteMembershipStorageBackend &membership_backend,
    PlatformIdentity *identity,
    SiteMembership *site_membership) {
  if (identity == nullptr)
    return PlatformIdentityStoreResult::STORAGE_FAILED;

  NodeIdentity node{};
  SiteMembership membership{};
  const auto node_result = NodeIdentityStore::load(node_backend, &node);
  const auto membership_result = SiteMembershipStore::load(membership_backend, &membership);
  const bool node_ready = node_result == NodeIdentityStoreResult::LOADED;
  const bool membership_ready = membership_result == SiteMembershipStoreResult::LOADED ||
                                membership_result == SiteMembershipStoreResult::COMMIT_REPAIRED;

  if (node_ready && membership_ready) {
    compose_platform_identity(node, membership, identity);
    if (site_membership != nullptr)
      *site_membership = membership;
    return membership_result == SiteMembershipStoreResult::COMMIT_REPAIRED
               ? PlatformIdentityStoreResult::MARKER_REPAIRED
               : PlatformIdentityStoreResult::LOADED;
  }
  if (node_result != NodeIdentityStoreResult::EMPTY && !node_ready)
    return map_node_failure(node_result);
  if (membership_result != SiteMembershipStoreResult::EMPTY && !membership_ready)
    return map_membership_failure(membership_result);

  PlatformIdentity legacy{};
  const auto legacy_result = PlatformIdentityStore::load(legacy_backend, &legacy);
  if (!legacy_result_is_ready(legacy_result)) {
    if (node_result == NodeIdentityStoreResult::EMPTY &&
        membership_result == SiteMembershipStoreResult::EMPTY)
      return legacy_result;
    return PlatformIdentityStoreResult::CORRUPT;
  }

  if (node_ready && std::strcmp(node.node_id, legacy.node_id) != 0)
    return PlatformIdentityStoreResult::CORRUPT;
  if (membership_ready && std::strcmp(membership.site_id, legacy.site_id) != 0)
    return PlatformIdentityStoreResult::CORRUPT;

  if (!node_ready) {
    std::strncpy(node.node_id, legacy.node_id, NodeIdentity::MAX_ID_LENGTH);
    node.node_id[NodeIdentity::MAX_ID_LENGTH] = '\0';
    const auto create_result = NodeIdentityStore::create(node_backend, node);
    if (create_result != NodeIdentityStoreResult::CREATED)
      return map_node_failure(create_result);
  }
  if (!membership_ready) {
    const auto create_result = SiteMembershipStore::create(
        membership_backend, legacy.site_id, SiteMembershipMode::STANDALONE);
    if (create_result != SiteMembershipStoreResult::CREATED)
      return map_membership_failure(create_result);
    const auto reload_result = SiteMembershipStore::load(membership_backend, &membership);
    if (reload_result != SiteMembershipStoreResult::LOADED)
      return map_membership_failure(reload_result);
  }

  compose_platform_identity(node, membership, identity);
  if (site_membership != nullptr)
    *site_membership = membership;
  return PlatformIdentityStoreResult::MIGRATED;
}

PlatformIdentityStoreResult PlatformStateStore::create(
    PlatformIdentityStorageBackend &legacy_backend,
    NodeIdentityStorageBackend &node_backend,
    SiteMembershipStorageBackend &membership_backend,
    const PlatformIdentity &identity) {
  // Keep the V1 record as a recovery journal until both V2 records are committed.
  const auto journal_result = PlatformIdentityStore::create(legacy_backend, identity);
  if (journal_result != PlatformIdentityStoreResult::CREATED)
    return journal_result;
  PlatformIdentity migrated{};
  const auto migrate_result = load_or_migrate(
      legacy_backend, node_backend, membership_backend, &migrated);
  if (migrate_result != PlatformIdentityStoreResult::MIGRATED)
    return migrate_result;
  return PlatformIdentityStoreResult::CREATED;
}

const char *platform_identity_store_result_to_string(PlatformIdentityStoreResult result) {
  switch (result) {
    case PlatformIdentityStoreResult::EMPTY:
      return "empty";
    case PlatformIdentityStoreResult::LOADED:
      return "loaded";
    case PlatformIdentityStoreResult::CREATED:
      return "created";
    case PlatformIdentityStoreResult::MIGRATED:
      return "migrated";
    case PlatformIdentityStoreResult::MARKER_REPAIRED:
      return "marker_repaired";
    case PlatformIdentityStoreResult::CORRUPT:
      return "corrupt";
    case PlatformIdentityStoreResult::UNSUPPORTED_VERSION:
      return "unsupported_version";
    case PlatformIdentityStoreResult::STORAGE_FAILED:
      return "storage_failed";
    case PlatformIdentityStoreResult::GENERATION_FAILED:
      return "generation_failed";
  }
  return "unknown";
}

std::string build_platform_status_json(const PlatformIdentity &identity,
                                       bool identity_ready, bool registry_ready,
                                       std::size_t remote_participant_count,
                                       std::size_t connected_remote_participant_count,
                                       std::size_t active_remote_participant_count,
                                       bool pairing_enabled,
                                       bool readiness_ready,
                                       std::size_t configured_participant_count,
                                       SiteMembershipMode membership_mode,
                                       bool fusion_enabled) {
  std::string json;
  json.reserve(448);
  const char *mode = identity_ready &&
                             membership_mode == SiteMembershipMode::ENROLLED
                         ? "enrolled"
                         : "standalone";
  if (identity_ready) {
    json.append("{\"schemaVersion\":1,\"identityReady\":true,\"siteId\":\"");
    json.append(identity.site_id);
    json.append("\",\"nodeId\":\"");
    json.append(identity.node_id);
    json.append("\",\"mode\":\"");
    json.append(mode);
    json.append("\",\"maxParticipants\":");
  } else {
    json.append("{\"schemaVersion\":1,\"identityReady\":false,\"mode\":\"");
    json.append(mode);
    json.append("\",\"maxParticipants\":");
  }

  const bool exposed_readiness_ready = identity_ready && readiness_ready;
  json.append(std::to_string(static_cast<unsigned>(ParticipantRegistry::MAX_PARTICIPANTS)));
  json.append(",\"registryReady\":");
  json.append(registry_ready ? "true" : "false");
  json.append(",\"remoteParticipantCount\":");
  json.append(std::to_string(static_cast<unsigned>(registry_ready ? remote_participant_count : 0)));
  json.append(",\"connectedRemoteParticipantCount\":");
  json.append(std::to_string(static_cast<unsigned>(
      registry_ready ? connected_remote_participant_count : 0)));
  json.append(",\"activeRemoteParticipantCount\":");
  json.append(std::to_string(
      static_cast<unsigned>(registry_ready ? active_remote_participant_count : 0)));
  json.append(",\"readinessReady\":");
  json.append(exposed_readiness_ready ? "true" : "false");
  json.append(",\"configuredRemoteParticipantCount\":");
  json.append(std::to_string(
      static_cast<unsigned>(exposed_readiness_ready ? configured_participant_count : 0)));
  json.append(",\"fusionEnabled\":");
  json.append(fusion_enabled ? "true" : "false");
  json.append(",\"pairingSupported\":true,\"pairingEnabled\":");
  json.append(pairing_enabled ? "true" : "false");
  json.push_back('}');
  return json;
}

std::string build_platform_descriptor_json(const PlatformIdentity &identity,
                                           bool identity_ready,
                                           const char *software_version) {
  if (!identity_ready || identity.node_id[0] == '\0' || software_version == nullptr ||
      software_version[0] == '\0')
    return {};

  char json[768];
  const int written = std::snprintf(
      json, sizeof(json),
      "{\"protocolVersion\":1,\"participantId\":\"%s\","
      "\"participantType\":\"namo_node\",\"product\":\"namo-spatial-sensor\","
      "\"softwareVersion\":\"%s\",\"capabilities\":{"
      "\"pointTracks\":true,\"velocity\":true,\"positionUncertainty\":true,"
      "\"range\":true,\"presence\":true,\"motion\":true,"
      "\"zonePresence\":false,\"bridge\":false,\"maxTargets\":3},"
      "\"protocolRange\":{\"minimum\":1,\"maximum\":1}}",
      identity.node_id, software_version);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(json))
    return {};
  return json;
}

#ifdef USE_ESP32

namespace {

constexpr uint32_t IDENTITY_PREFERENCE_KEY = 0xF3468E8EU;
constexpr uint32_t MARKER_PREFERENCE_KEY = 0xE1874234U;
constexpr uint32_t NODE_IDENTITY_PREFERENCE_KEY = 0x562676D1U;
constexpr uint32_t SITE_MEMBERSHIP_SLOT_0_KEY = 0x25EA80CAU;
constexpr uint32_t SITE_MEMBERSHIP_SLOT_1_KEY = 0x52EDB05CU;
constexpr uint32_t SITE_MEMBERSHIP_COMMIT_KEY = 0xCC836DFFU;
const char *const TAG = "platform_identity";

class EspHomePlatformIdentityBackend : public PlatformIdentityStorageBackend,
                                       public NodeIdentityStorageBackend,
                                       public SiteMembershipStorageBackend {
 public:
  bool initialize() {
    if (this->initialized_)
      return true;
    if (global_preferences == nullptr)
      return false;
    this->identity_preference_ =
        global_preferences->make_preference<PlatformIdentityStorageBlob>(
            IDENTITY_PREFERENCE_KEY, true);
    this->marker_preference_ = global_preferences->make_preference<uint32_t>(
        MARKER_PREFERENCE_KEY, true);
    this->node_identity_preference_ =
        global_preferences->make_preference<NodeIdentityStorageBlob>(
            NODE_IDENTITY_PREFERENCE_KEY, true);
    this->site_membership_slot_preferences_[0] =
        global_preferences->make_preference<SiteMembershipStorageBlob>(
            SITE_MEMBERSHIP_SLOT_0_KEY, true);
    this->site_membership_slot_preferences_[1] =
        global_preferences->make_preference<SiteMembershipStorageBlob>(
            SITE_MEMBERSHIP_SLOT_1_KEY, true);
    this->site_membership_commit_preference_ =
        global_preferences->make_preference<SiteMembershipCommitBlob>(
            SITE_MEMBERSHIP_COMMIT_KEY, true);
    this->initialized_ = true;
    return true;
  }

  bool load_identity(PlatformIdentityStorageBlob *blob) override {
    return this->identity_preference_.load(blob);
  }
  bool load_marker(uint32_t *marker) override {
    return this->marker_preference_.load(marker);
  }
  bool save_identity(const PlatformIdentityStorageBlob &blob) override {
    return this->identity_preference_.save(&blob);
  }
  bool save_marker(uint32_t marker) override {
    return this->marker_preference_.save(&marker);
  }
  bool load_node_identity(NodeIdentityStorageBlob *blob) override {
    return this->node_identity_preference_.load(blob);
  }
  bool save_node_identity(const NodeIdentityStorageBlob &blob) override {
    return this->node_identity_preference_.save(&blob);
  }
  bool load_site_membership_slot(uint8_t slot,
                                 SiteMembershipStorageBlob *blob) override {
    return slot < 2 && this->site_membership_slot_preferences_[slot].load(blob);
  }
  bool save_site_membership_slot(uint8_t slot,
                                 const SiteMembershipStorageBlob &blob) override {
    return slot < 2 && this->site_membership_slot_preferences_[slot].save(&blob);
  }
  bool load_site_membership_commit(SiteMembershipCommitBlob *blob) override {
    return this->site_membership_commit_preference_.load(blob);
  }
  bool save_site_membership_commit(const SiteMembershipCommitBlob &blob) override {
    return this->site_membership_commit_preference_.save(&blob);
  }
  bool sync() override { return global_preferences->sync(); }

 private:
  bool initialized_{false};
  ESPPreferenceObject identity_preference_{};
  ESPPreferenceObject marker_preference_{};
  ESPPreferenceObject node_identity_preference_{};
  ESPPreferenceObject site_membership_slot_preferences_[2]{};
  ESPPreferenceObject site_membership_commit_preference_{};
};

EspHomePlatformIdentityBackend &esp32_platform_identity_backend() {
  static EspHomePlatformIdentityBackend backend;
  return backend;
}

}  // namespace

PlatformIdentityStoreResult initialize_esp32_platform_identity(
    PlatformIdentity *identity, SiteMembership *site_membership) {
  if (identity == nullptr || site_membership == nullptr || global_preferences == nullptr)
    return PlatformIdentityStoreResult::STORAGE_FAILED;

  auto &backend = esp32_platform_identity_backend();
  if (!backend.initialize())
    return PlatformIdentityStoreResult::STORAGE_FAILED;

  const auto load_result = PlatformStateStore::load_or_migrate(
      backend, backend, backend, identity, site_membership);
  if (load_result != PlatformIdentityStoreResult::EMPTY)
    return load_result;

  uint8_t mac[6]{};
  uint8_t site_random[16]{};
  get_mac_address_raw(mac);
  if (!random_bytes(site_random, sizeof(site_random)))
    return PlatformIdentityStoreResult::GENERATION_FAILED;

  PlatformIdentity generated{};
  if (create_initial_platform_identity(mac, site_random, &generated) !=
      PlatformIdentityCodecResult::OK)
    return PlatformIdentityStoreResult::GENERATION_FAILED;

  const auto create_result = PlatformStateStore::create(backend, backend, backend, generated);
  if (create_result == PlatformIdentityStoreResult::CREATED) {
    *identity = generated;
    std::strncpy(site_membership->site_id, generated.site_id,
                 SiteMembership::MAX_ID_LENGTH);
    site_membership->site_id[SiteMembership::MAX_ID_LENGTH] = '\0';
    site_membership->mode = SiteMembershipMode::STANDALONE;
    site_membership->generation = 1;
    ESP_LOGI(TAG, "Created stable Site and Node identity");
  }
  return create_result;
}

bool update_esp32_site_membership(const char *site_id, SiteMembershipMode mode,
                                  PlatformIdentity *identity,
                                  SiteMembership *site_membership) {
  if (site_id == nullptr || identity == nullptr || site_membership == nullptr)
    return false;
  auto &backend = esp32_platform_identity_backend();
  if (!backend.initialize() ||
      SiteMembershipStore::update(backend, site_id, mode) !=
          SiteMembershipStoreResult::UPDATED)
    return false;
  SiteMembership loaded{};
  const auto load_result = SiteMembershipStore::load(backend, &loaded);
  if (load_result != SiteMembershipStoreResult::LOADED &&
      load_result != SiteMembershipStoreResult::COMMIT_REPAIRED)
    return false;
  std::strncpy(identity->site_id, loaded.site_id,
               PlatformIdentity::MAX_ID_LENGTH);
  identity->site_id[PlatformIdentity::MAX_ID_LENGTH] = '\0';
  *site_membership = loaded;
  return true;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
