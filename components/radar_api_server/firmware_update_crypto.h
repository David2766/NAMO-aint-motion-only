#pragma once

#include "firmware_update_security.h"

#ifdef USE_ESP32

namespace esphome {
namespace radar_api_server {

class Esp32FirmwareSignatureVerifier : public FirmwareSignatureVerifier {
 public:
  bool validate_es256_public_key(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
          &public_key,
      const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
          &expected_key_id) override;
  bool verify_es256_p1363(
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
          &public_key,
      const uint8_t *message, std::size_t message_size,
      const std::array<uint8_t, FirmwarePackageHeader::SIGNATURE_SIZE>
          &signature) override;
};

bool build_embedded_firmware_trust(FirmwareTrustState *state);
FirmwareTrustStoreResult initialize_esp32_firmware_trust(
    FirmwareTrustSnapshot *snapshot,
    FirmwareTrustStorageBackend **backend,
    FirmwareTrustStore *store);

}  // namespace radar_api_server
}  // namespace esphome

#endif
