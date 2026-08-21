#pragma once

#include "firmware_update_session.h"

#include "esphome/components/web_server_base/web_server_base.h"

#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
#include "firmware_update_crypto.h"
#include "esphome/components/ota/ota_backend_factory.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

class RadarApiServer;

class FirmwareUpdateHandler : public FirmwareUploadSink {
 public:
  explicit FirmwareUpdateHandler(RadarApiServer *server) : server_(server) {}

  bool setup();
  static bool can_handle(AsyncWebServerRequest *request, StringRef url);
  static bool can_handle_upload(AsyncWebServerRequest *request,
                                StringRef url);
  bool handle(AsyncWebServerRequest *request, StringRef url);
  void handle_upload(AsyncWebServerRequest *request, StringRef url,
                     const std::string &filename, std::size_t index,
                     uint8_t *data, std::size_t size, bool final);

  bool begin_firmware_image(uint32_t image_size) override;
  bool write_firmware_image(const uint8_t *data,
                            std::size_t size) override;
  bool finish_firmware_image(
      const FirmwarePackageHeader &package,
      const FirmwareTrustDecision &trust) override;
  void abort_firmware_image() override;

 private:
  void handle_status_(AsyncWebServerRequest *request);
  void handle_alpha_permission_(AsyncWebServerRequest *request);
  void handle_start_(AsyncWebServerRequest *request);
  void handle_cancel_(AsyncWebServerRequest *request);
  void handle_upload_result_(AsyncWebServerRequest *request);
  void expire_stale_session_();
  bool decode_header_(const std::string &hex,
                      std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE>
                          *header) const;

  RadarApiServer *server_;
  FirmwareUploadSession session_{};
  FirmwareTrustStore trust_store_{};
  FirmwareTrustSnapshot trust_snapshot_{};
  FirmwareTrustStorageBackend *trust_backend_{nullptr};
  FirmwareTrustStoreResult trust_store_result_{
      FirmwareTrustStoreResult::EMPTY};
  FirmwareUploadResult upload_result_{FirmwareUploadResult::NOT_ACTIVE};
  FirmwareDeveloperInstallGate developer_install_gate_{};
  bool upload_success_{false};
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  Esp32FirmwareSignatureVerifier verifier_{};
  ota::OTABackendPtr ota_backend_{nullptr};
  const esp_partition_t *update_partition_{nullptr};
  std::array<uint8_t, 1024> verify_buffer_{};
#endif
};

static_assert(sizeof(FirmwareUpdateHandler) <= 6144,
              "firmware update handler exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
