#pragma once

#include "platform_identity.h"
#include "site_membership.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct PlatformIdentityStorageBlob {
  std::array<uint8_t, PlatformIdentityCodec::MAX_ENCODED_SIZE> bytes{};
};

struct NodeIdentityStorageBlob {
  std::array<uint8_t, NodeIdentityCodec::MAX_ENCODED_SIZE> bytes{};
};

enum class PlatformIdentityStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  MIGRATED,
  MARKER_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  GENERATION_FAILED,
};

enum class NodeIdentityStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  GENERATION_FAILED,
};

class PlatformIdentityStorageBackend {
 public:
  virtual ~PlatformIdentityStorageBackend() = default;
  virtual bool load_identity(PlatformIdentityStorageBlob *blob) = 0;
  virtual bool load_marker(uint32_t *marker) = 0;
  virtual bool save_identity(const PlatformIdentityStorageBlob &blob) = 0;
  virtual bool save_marker(uint32_t marker) = 0;
  virtual bool sync() = 0;
};

class NodeIdentityStorageBackend {
 public:
  virtual ~NodeIdentityStorageBackend() = default;
  virtual bool load_node_identity(NodeIdentityStorageBlob *blob) = 0;
  virtual bool save_node_identity(const NodeIdentityStorageBlob &blob) = 0;
  virtual bool sync() = 0;
};

class PlatformIdentityStore {
 public:
  static constexpr uint32_t INITIALIZED_MARKER = 0x4E414D31U;  // NAM1

  static PlatformIdentityStoreResult load(PlatformIdentityStorageBackend &backend,
                                          PlatformIdentity *identity);
  static PlatformIdentityStoreResult create(PlatformIdentityStorageBackend &backend,
                                            const PlatformIdentity &identity);
};

class NodeIdentityStore {
 public:
  static NodeIdentityStoreResult load(NodeIdentityStorageBackend &backend,
                                      NodeIdentity *identity);
  static NodeIdentityStoreResult create(NodeIdentityStorageBackend &backend,
                                        const NodeIdentity &identity);
};

class PlatformStateStore {
 public:
  static PlatformIdentityStoreResult load_or_migrate(
      PlatformIdentityStorageBackend &legacy_backend,
      NodeIdentityStorageBackend &node_backend,
      SiteMembershipStorageBackend &membership_backend,
      PlatformIdentity *identity,
      SiteMembership *site_membership = nullptr);
  static PlatformIdentityStoreResult create(
      PlatformIdentityStorageBackend &legacy_backend,
      NodeIdentityStorageBackend &node_backend,
      SiteMembershipStorageBackend &membership_backend,
      const PlatformIdentity &identity);
};

const char *platform_identity_store_result_to_string(PlatformIdentityStoreResult result);
std::string build_platform_status_json(const PlatformIdentity &identity, bool identity_ready,
                                       bool registry_ready = false,
                                       std::size_t remote_participant_count = 0,
                                       std::size_t connected_remote_participant_count = 0,
                                       std::size_t active_remote_participant_count = 0,
                                       bool pairing_enabled = false,
                                       bool readiness_ready = false,
                                       std::size_t configured_participant_count = 0,
                                       SiteMembershipMode membership_mode =
                                           SiteMembershipMode::STANDALONE,
                                       bool fusion_enabled = false);
std::string build_platform_descriptor_json(const PlatformIdentity &identity,
                                           bool identity_ready,
                                           const char *software_version);

#ifdef USE_ESP32
PlatformIdentityStoreResult initialize_esp32_platform_identity(
    PlatformIdentity *identity, SiteMembership *site_membership);
bool update_esp32_site_membership(const char *site_id, SiteMembershipMode mode,
                                  PlatformIdentity *identity,
                                  SiteMembership *site_membership);
#endif

static_assert(std::is_trivially_copyable<PlatformIdentityStorageBlob>::value,
              "identity preference storage must remain trivially copyable");
static_assert(std::is_trivially_copyable<NodeIdentityStorageBlob>::value,
              "node identity preference storage must remain trivially copyable");

}  // namespace radar_api_server
}  // namespace esphome
