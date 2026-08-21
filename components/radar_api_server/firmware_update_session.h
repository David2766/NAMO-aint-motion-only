#pragma once

#include "firmware_update_security.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

enum class FirmwareUploadResult : uint8_t {
  OK = 0,
  COMPLETE,
  BUSY,
  NOT_ACTIVE,
  SESSION_MISMATCH,
  EXPIRED,
  OFFSET_MISMATCH,
  HEADER_MISMATCH,
  SIZE_MISMATCH,
  TRUST_REJECTED,
  SINK_BEGIN_FAILED,
  SINK_WRITE_FAILED,
  SINK_FINISH_FAILED,
};

enum class FirmwareMultipartMarker : uint8_t {
  NONE = 0,
  START,
  END,
};

FirmwareMultipartMarker classify_firmware_multipart_marker(
    std::size_t index, const uint8_t *data, std::size_t size, bool final);

class FirmwareUploadSink {
 public:
  virtual ~FirmwareUploadSink() = default;
  virtual bool begin_firmware_image(uint32_t image_size) = 0;
  virtual bool write_firmware_image(const uint8_t *data,
                                    std::size_t size) = 0;
  virtual bool finish_firmware_image(
      const FirmwarePackageHeader &package,
      const FirmwareTrustDecision &trust) = 0;
  virtual void abort_firmware_image() = 0;
};

class FirmwareUploadSession {
 public:
  static constexpr uint32_t TIMEOUT_MS = 2 * 60 * 1000;

  FirmwareUploadResult start(
      const uint8_t *header, std::size_t header_size, uint32_t session_id,
      uint32_t now_ms, const FirmwareTrustState &trust,
      const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
          &root_key_id,
      const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
          &root_public_key,
      FirmwareSignatureVerifier &verifier);
  FirmwareUploadResult consume(uint32_t session_id, std::size_t index,
                               const uint8_t *data, std::size_t size,
                               bool final, uint32_t now_ms,
                               FirmwareUploadSink &sink);
  FirmwareUploadResult finish(uint32_t session_id, std::size_t index,
                              uint32_t now_ms, FirmwareUploadSink &sink);
  bool expire(uint32_t now_ms, FirmwareUploadSink &sink);
  bool cancel(uint32_t session_id, FirmwareUploadSink &sink);

  bool active() const { return this->active_; }
  uint32_t session_id() const { return this->session_id_; }
  uint32_t image_size() const { return this->package_.manifest.image_size; }
  std::size_t received_size() const { return this->received_size_; }
  const FirmwarePackageHeader &package() const { return this->package_; }
  const FirmwareTrustDecision &trust_decision() const {
    return this->trust_decision_;
  }

 private:
  FirmwareUploadResult fail_(FirmwareUploadResult result,
                             FirmwareUploadSink &sink);

  FirmwarePackageHeader package_{};
  FirmwareTrustDecision trust_decision_{};
  uint32_t session_id_{0};
  uint32_t last_activity_ms_{0};
  std::size_t received_size_{0};
  bool active_{false};
  bool sink_started_{false};
};

const char *firmware_upload_result_to_string(FirmwareUploadResult result);

static_assert(sizeof(FirmwareUploadSession) <= 1536,
              "firmware upload session exceeded its embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
