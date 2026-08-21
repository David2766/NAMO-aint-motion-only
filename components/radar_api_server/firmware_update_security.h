#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class FirmwareSignerRole : uint8_t {
  RELEASE = 1,
  DEVELOPER = 2,
  ROOT_RECOVERY = 3,
};

class FirmwareDeveloperInstallGate {
 public:
  static constexpr uint32_t WINDOW_MS = 10U * 60U * 1000U;

  void set_enabled(bool enabled, uint32_t now_ms) {
    this->enabled_ = enabled;
    this->enabled_at_ms_ = enabled ? now_ms : 0;
  }
  bool allowed(uint32_t now_ms) const {
    return this->enabled_ && now_ms - this->enabled_at_ms_ < WINDOW_MS;
  }
  uint32_t remaining_seconds(uint32_t now_ms) const {
    if (!this->allowed(now_ms))
      return 0;
    const uint32_t remaining_ms =
        WINDOW_MS - (now_ms - this->enabled_at_ms_);
    return (remaining_ms + 999U) / 1000U;
  }
  void consume() { this->set_enabled(false, 0); }

 private:
  uint32_t enabled_at_ms_{0};
  bool enabled_{false};
};

enum class FirmwareSignatureAlgorithm : uint8_t {
  ES256_P1363 = 1,
};

enum class FirmwarePackageResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_MAGIC,
  UNSUPPORTED_FORMAT,
  UNSUPPORTED_ALGORITHM,
  INVALID_ROLE,
  INVALID_PRODUCT,
  INVALID_HARDWARE,
  INVALID_IMAGE_SIZE,
  INVALID_TEXT,
  INVALID_RECOVERY,
  INVALID_RESERVED,
};

enum class FirmwareTrustResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  TRUST_UNAVAILABLE,
  UNKNOWN_KEY,
  REVOKED_KEY,
  ROLE_MISMATCH,
  TRUST_REVISION_MISMATCH,
  TRUST_ROLLBACK,
  SECURITY_ROLLBACK,
  SECURITY_EPOCH_MISMATCH,
  INVALID_SIGNATURE,
  INVALID_RECOVERY,
  KEY_CAPACITY_EXCEEDED,
};

struct FirmwarePackageManifest {
  static constexpr std::size_t KEY_ID_SIZE = 16;
  static constexpr std::size_t DIGEST_SIZE = 32;
  static constexpr std::size_t PUBLIC_KEY_SIZE = 65;
  static constexpr std::size_t VERSION_SIZE = 24;
  static constexpr std::size_t BUILD_SIZE = 24;

  FirmwareSignerRole signer_role{FirmwareSignerRole::RELEASE};
  FirmwareSignatureAlgorithm signature_algorithm{
      FirmwareSignatureAlgorithm::ES256_P1363};
  uint16_t flags{0};
  uint32_t product_id{0};
  uint32_t hardware_id{0};
  uint32_t image_size{0};
  uint32_t trust_revision{0};
  uint32_t security_epoch{0};
  std::array<uint8_t, KEY_ID_SIZE> signer_key_id{};
  std::array<uint8_t, DIGEST_SIZE> image_sha256{};
  std::array<char, VERSION_SIZE> firmware_version{};
  std::array<char, BUILD_SIZE> build_id{};
  std::array<uint8_t, KEY_ID_SIZE> revoked_key_id{};
  FirmwareSignerRole replacement_key_role{FirmwareSignerRole::RELEASE};
  std::array<uint8_t, KEY_ID_SIZE> replacement_key_id{};
  std::array<uint8_t, PUBLIC_KEY_SIZE> replacement_public_key{};
};

struct FirmwarePackageHeader {
  static constexpr std::size_t SIGNED_SIZE = 256;
  static constexpr std::size_t SIGNATURE_SIZE = 64;
  static constexpr std::size_t HEADER_SIZE =
      SIGNED_SIZE + SIGNATURE_SIZE;
  static constexpr uint16_t FORMAT_VERSION = 1;
  static constexpr uint32_t PRODUCT_ID = 0x3153504EU;   // "NPS1"
  static constexpr uint32_t HARDWARE_ID = 0x31335358U;  // "XS31"
  static constexpr uint32_t MAX_IMAGE_SIZE = 0x300000U;

  FirmwarePackageManifest manifest{};
  std::array<uint8_t, SIGNATURE_SIZE> signature{};
  std::array<uint8_t, SIGNED_SIZE> signed_bytes{};
};

FirmwarePackageResult parse_firmware_package_header(
    const uint8_t *data, std::size_t size, FirmwarePackageHeader *output);
const char *firmware_package_result_to_string(FirmwarePackageResult result);

struct FirmwareTrustKey {
  bool occupied{false};
  FirmwareSignerRole role{FirmwareSignerRole::RELEASE};
  std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE> key_id{};
  std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> public_key{};
};

struct FirmwareTrustState {
  static constexpr std::size_t MAX_RELEASE_KEYS = 2;
  static constexpr std::size_t MAX_DEVELOPER_KEYS = 2;
  static constexpr std::size_t MAX_REVOKED_KEYS = 4;

  uint32_t trust_revision{0};
  uint32_t minimum_security_epoch{0};
  std::array<FirmwareTrustKey, MAX_RELEASE_KEYS> release_keys{};
  std::array<FirmwareTrustKey, MAX_DEVELOPER_KEYS> developer_keys{};
  std::array<std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>,
             MAX_REVOKED_KEYS>
      revoked_key_ids{};
};

class FirmwareSignatureVerifier {
 public:
  virtual ~FirmwareSignatureVerifier() = default;
  virtual bool validate_es256_public_key(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
          &public_key,
      const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
          &expected_key_id) = 0;
  virtual bool verify_es256_p1363(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
          &public_key,
      const uint8_t *message, std::size_t message_size,
      const std::array<uint8_t, FirmwarePackageHeader::SIGNATURE_SIZE>
          &signature) = 0;
};

struct FirmwareTrustDecision {
  FirmwareTrustResult result{FirmwareTrustResult::INVALID_ARGUMENT};
  bool trust_update_required{false};
  FirmwareTrustState next_trust{};

  bool accepted() const { return this->result == FirmwareTrustResult::OK; }
};

FirmwareTrustDecision evaluate_firmware_package_trust(
    const FirmwarePackageHeader &package, const FirmwareTrustState &current,
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
        &root_key_id,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &root_public_key,
    FirmwareSignatureVerifier &verifier);
const char *firmware_trust_result_to_string(FirmwareTrustResult result);

enum class FirmwareTrustStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  STAGED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_STATE,
  ROLLBACK_REJECTED,
};

struct FirmwareTrustStorageBlob {
  static constexpr std::size_t SIZE = 1024;
  static constexpr std::size_t ENCODED_SIZE =
      8 + 2 + 2 + 4 + 4 + 4 +
      (FirmwareTrustState::MAX_RELEASE_KEYS +
       FirmwareTrustState::MAX_DEVELOPER_KEYS) * (1 + 1 + 2 + 16 + 65) +
      FirmwareTrustState::MAX_REVOKED_KEYS * 16 + 4;
  std::array<uint8_t, SIZE> bytes{};
};

struct FirmwareTrustCommitBlob {
  static constexpr std::size_t SIZE = 24;
  std::array<uint8_t, SIZE> bytes{};
};

struct FirmwareTrustSnapshot {
  uint32_t generation{0};
  FirmwareTrustState state{};
};

struct FirmwareTrustStagedUpdate {
  bool valid{false};
  uint8_t slot{0};
  uint32_t generation{0};
  uint32_t trust_revision{0};
};

class FirmwareTrustStorageBackend {
 public:
  virtual ~FirmwareTrustStorageBackend() = default;
  virtual bool load_firmware_trust_slot(uint8_t slot,
                                        FirmwareTrustStorageBlob *blob) = 0;
  virtual bool save_firmware_trust_slot(
      uint8_t slot, const FirmwareTrustStorageBlob &blob) = 0;
  virtual bool load_firmware_trust_commit(
      FirmwareTrustCommitBlob *blob) = 0;
  virtual bool save_firmware_trust_commit(
      const FirmwareTrustCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class FirmwareTrustStore {
 public:
  FirmwareTrustStoreResult load(FirmwareTrustStorageBackend &backend,
                                FirmwareTrustSnapshot *snapshot);
  FirmwareTrustStoreResult create(FirmwareTrustStorageBackend &backend,
                                  const FirmwareTrustState &initial,
                                  FirmwareTrustSnapshot *snapshot);
  FirmwareTrustStoreResult update(FirmwareTrustStorageBackend &backend,
                                  const FirmwareTrustState &next,
                                  FirmwareTrustSnapshot *snapshot);
  FirmwareTrustStoreResult stage_update(
      FirmwareTrustStorageBackend &backend, const FirmwareTrustState &next,
      const FirmwareTrustSnapshot &snapshot,
      FirmwareTrustStagedUpdate *staged);
  FirmwareTrustStoreResult commit_staged(
      FirmwareTrustStorageBackend &backend,
      const FirmwareTrustStagedUpdate &staged,
      FirmwareTrustSnapshot *snapshot);

 private:
  FirmwareTrustStorageBlob blob_{};

  FirmwareTrustStoreResult load_slot_(FirmwareTrustStorageBackend &backend,
                                      uint8_t slot,
                                      FirmwareTrustSnapshot *snapshot);
  FirmwareTrustStoreResult write_(FirmwareTrustStorageBackend &backend,
                                  uint8_t slot,
                                  const FirmwareTrustState &state,
                                  uint32_t generation);
  FirmwareTrustStoreResult write_slot_(FirmwareTrustStorageBackend &backend,
                                       uint8_t slot,
                                       const FirmwareTrustState &state,
                                       uint32_t generation);
};

const char *firmware_trust_store_result_to_string(
    FirmwareTrustStoreResult result);

static_assert(std::is_trivially_copyable<FirmwareTrustState>::value,
              "firmware trust state must remain fixed-size");
static_assert(sizeof(FirmwareTrustState) <= 512,
              "firmware trust state exceeded its embedded budget");
static_assert(FirmwarePackageHeader::HEADER_SIZE == 320,
              "firmware package header contract changed unexpectedly");
static_assert(FirmwareTrustStorageBlob::ENCODED_SIZE <=
                  FirmwareTrustStorageBlob::SIZE,
              "firmware trust encoding exceeded its slot");

}  // namespace radar_api_server
}  // namespace esphome
