#include "firmware_update_crypto.h"

#ifdef USE_ESP32

#include "firmware_trust_keys.h"

#include "esphome/core/preferences.h"

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

#include <algorithm>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint32_t TRUST_SLOT_0_KEY = 0x7E814C31U;
constexpr uint32_t TRUST_SLOT_1_KEY = 0xC21C6E8AU;
constexpr uint32_t TRUST_COMMIT_KEY = 0xA5F03B19U;

class Esp32FirmwareTrustStorage : public FirmwareTrustStorageBackend {
 public:
  bool initialize() {
    if (this->initialized_)
      return true;
    if (global_preferences == nullptr)
      return false;
    this->slots_[0] =
        global_preferences->make_preference<FirmwareTrustStorageBlob>(
            TRUST_SLOT_0_KEY, true);
    this->slots_[1] =
        global_preferences->make_preference<FirmwareTrustStorageBlob>(
            TRUST_SLOT_1_KEY, true);
    this->commit_ =
        global_preferences->make_preference<FirmwareTrustCommitBlob>(
            TRUST_COMMIT_KEY, true);
    this->initialized_ = true;
    return true;
  }

  bool load_firmware_trust_slot(uint8_t slot,
                                FirmwareTrustStorageBlob *blob) override {
    return slot < 2 && blob != nullptr && this->slots_[slot].load(blob);
  }
  bool save_firmware_trust_slot(
      uint8_t slot, const FirmwareTrustStorageBlob &blob) override {
    return slot < 2 && this->slots_[slot].save(&blob);
  }
  bool load_firmware_trust_commit(
      FirmwareTrustCommitBlob *blob) override {
    return blob != nullptr && this->commit_.load(blob);
  }
  bool save_firmware_trust_commit(
      const FirmwareTrustCommitBlob &blob) override {
    return this->commit_.save(&blob);
  }
  bool sync() override {
    return global_preferences != nullptr && global_preferences->sync();
  }

 private:
  bool initialized_{false};
  ESPPreferenceObject slots_[2]{};
  ESPPreferenceObject commit_{};
};

Esp32FirmwareTrustStorage trust_backend;

}  // namespace

bool Esp32FirmwareSignatureVerifier::validate_es256_public_key(
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &public_key,
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
        &expected_key_id) {
  static constexpr std::array<uint8_t, 26> P256_SPKI_PREFIX{
      0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
      0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
      0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
  mbedtls_ecp_group group;
  mbedtls_ecp_point point;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&point);
  bool valid =
      mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_point_read_binary(&group, &point, public_key.data(),
                                    public_key.size()) == 0 &&
      mbedtls_ecp_check_pubkey(&group, &point) == 0;
  mbedtls_ecp_point_free(&point);
  mbedtls_ecp_group_free(&group);
  std::array<uint8_t, P256_SPKI_PREFIX.size() +
                          FirmwarePackageManifest::PUBLIC_KEY_SIZE>
      spki{};
  std::array<uint8_t, FirmwarePackageManifest::DIGEST_SIZE> digest{};
  std::copy(P256_SPKI_PREFIX.begin(), P256_SPKI_PREFIX.end(), spki.begin());
  std::copy(public_key.begin(), public_key.end(),
            spki.begin() + P256_SPKI_PREFIX.size());
  const auto *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  valid = valid && md != nullptr &&
          mbedtls_md(md, spki.data(), spki.size(), digest.data()) == 0;
  uint8_t difference = 0;
  for (std::size_t index = 0; index < expected_key_id.size(); ++index)
    difference |= digest[index] ^ expected_key_id[index];
  valid = valid && difference == 0;
  return valid;
}

bool Esp32FirmwareSignatureVerifier::verify_es256_p1363(
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &public_key,
    const uint8_t *message, std::size_t message_size,
    const std::array<uint8_t, FirmwarePackageHeader::SIGNATURE_SIZE>
        &signature) {
  if (message == nullptr || message_size == 0)
    return false;
  std::array<uint8_t, FirmwarePackageManifest::DIGEST_SIZE> digest{};
  const auto *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr ||
      mbedtls_md(md, message, message_size, digest.data()) != 0)
    return false;

  mbedtls_ecp_group group;
  mbedtls_ecp_point point;
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&point);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  const bool verified =
      mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_point_read_binary(&group, &point, public_key.data(),
                                    public_key.size()) == 0 &&
      mbedtls_ecp_check_pubkey(&group, &point) == 0 &&
      mbedtls_mpi_read_binary(&r, signature.data(), 32) == 0 &&
      mbedtls_mpi_read_binary(&s, signature.data() + 32, 32) == 0 &&
      mbedtls_ecdsa_verify(&group, digest.data(), digest.size(), &point, &r,
                           &s) == 0;
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&point);
  mbedtls_ecp_group_free(&group);
  return verified;
}

bool build_embedded_firmware_trust(FirmwareTrustState *state) {
  if (state == nullptr || !firmware_trust_keys::PROVISIONED ||
      firmware_trust_keys::TRUST_REVISION == 0 ||
      firmware_trust_keys::SECURITY_EPOCH == 0)
    return false;
  FirmwareTrustState initial{};
  initial.trust_revision = firmware_trust_keys::TRUST_REVISION;
  initial.minimum_security_epoch = firmware_trust_keys::SECURITY_EPOCH;
  initial.release_keys[0].occupied = true;
  initial.release_keys[0].role = FirmwareSignerRole::RELEASE;
  initial.release_keys[0].key_id = firmware_trust_keys::RELEASE_KEY_ID;
  initial.release_keys[0].public_key =
      firmware_trust_keys::RELEASE_PUBLIC_KEY;
  initial.developer_keys[0].occupied = true;
  initial.developer_keys[0].role = FirmwareSignerRole::DEVELOPER;
  initial.developer_keys[0].key_id = firmware_trust_keys::DEVELOPER_KEY_ID;
  initial.developer_keys[0].public_key =
      firmware_trust_keys::DEVELOPER_PUBLIC_KEY;
  *state = initial;
  return true;
}

FirmwareTrustStoreResult initialize_esp32_firmware_trust(
    FirmwareTrustSnapshot *snapshot,
    FirmwareTrustStorageBackend **backend,
    FirmwareTrustStore *store) {
  if (snapshot == nullptr || backend == nullptr || store == nullptr ||
      !trust_backend.initialize())
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  *backend = &trust_backend;
  auto result = store->load(trust_backend, snapshot);
  if (result != FirmwareTrustStoreResult::EMPTY)
    return result;
  FirmwareTrustState initial{};
  if (!build_embedded_firmware_trust(&initial))
    return FirmwareTrustStoreResult::INVALID_STATE;
  return store->create(trust_backend, initial, snapshot);
}

}  // namespace radar_api_server
}  // namespace esphome

#endif
