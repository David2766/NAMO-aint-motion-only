#pragma once

#include "platform_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class SiteMembershipMode : uint8_t {
  STANDALONE = 0,
  ENROLLED = 1,
};

struct SiteMembership {
  static constexpr std::size_t MAX_ID_LENGTH = PlatformIdentity::MAX_ID_LENGTH;

  char site_id[MAX_ID_LENGTH + 1]{};
  SiteMembershipMode mode{SiteMembershipMode::STANDALONE};
  uint32_t generation{0};
};

bool site_membership_matches_target(const SiteMembership &membership,
                                    const char *site_id,
                                    SiteMembershipMode mode);

enum class SiteMembershipCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_MEMBERSHIP,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class SiteMembershipCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t MAX_ENCODED_SIZE = 80;

  static SiteMembershipCodecResult encode(const SiteMembership &membership, uint8_t *output,
                                          std::size_t output_capacity,
                                          std::size_t *output_size);
  static SiteMembershipCodecResult decode(const uint8_t *data, std::size_t size,
                                          SiteMembership *membership);
};

struct SiteMembershipStorageBlob {
  std::array<uint8_t, SiteMembershipCodec::MAX_ENCODED_SIZE> bytes{};
};

struct SiteMembershipCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

enum class SiteMembershipStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_MEMBERSHIP,
};

class SiteMembershipStorageBackend {
 public:
  virtual ~SiteMembershipStorageBackend() = default;
  virtual bool load_site_membership_slot(uint8_t slot, SiteMembershipStorageBlob *blob) = 0;
  virtual bool save_site_membership_slot(uint8_t slot,
                                         const SiteMembershipStorageBlob &blob) = 0;
  virtual bool load_site_membership_commit(SiteMembershipCommitBlob *blob) = 0;
  virtual bool save_site_membership_commit(const SiteMembershipCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class SiteMembershipStore {
 public:
  static SiteMembershipStoreResult load(SiteMembershipStorageBackend &backend,
                                        SiteMembership *membership);
  static SiteMembershipStoreResult create(SiteMembershipStorageBackend &backend,
                                          const char *site_id,
                                          SiteMembershipMode mode);
  static SiteMembershipStoreResult update(SiteMembershipStorageBackend &backend,
                                          const char *site_id,
                                          SiteMembershipMode mode);
};

static_assert(std::is_trivially_copyable<SiteMembership>::value,
              "site membership must not own heap-backed state");
static_assert(sizeof(SiteMembership) <= 72,
              "site membership runtime state exceeded its embedded budget");
static_assert(std::is_trivially_copyable<SiteMembershipStorageBlob>::value,
              "site membership storage must remain trivially copyable");

}  // namespace radar_api_server
}  // namespace esphome
