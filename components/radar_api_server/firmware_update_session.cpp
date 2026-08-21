#include "firmware_update_session.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace radar_api_server {

FirmwareMultipartMarker classify_firmware_multipart_marker(
    std::size_t index, const uint8_t *data, std::size_t size, bool final) {
  if (data != nullptr || size != 0)
    return FirmwareMultipartMarker::NONE;
  if (!final && index == 0)
    return FirmwareMultipartMarker::START;
  if (final)
    return FirmwareMultipartMarker::END;
  return FirmwareMultipartMarker::NONE;
}

FirmwareUploadResult FirmwareUploadSession::start(
    const uint8_t *header, std::size_t header_size, uint32_t session_id,
    uint32_t now_ms, const FirmwareTrustState &trust,
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
        &root_key_id,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &root_public_key,
    FirmwareSignatureVerifier &verifier) {
  if (this->active_)
    return FirmwareUploadResult::BUSY;
  if (session_id == 0)
    return FirmwareUploadResult::SESSION_MISMATCH;
  FirmwarePackageHeader parsed{};
  if (header_size != FirmwarePackageHeader::HEADER_SIZE ||
      parse_firmware_package_header(header, header_size, &parsed) !=
      FirmwarePackageResult::OK)
    return FirmwareUploadResult::HEADER_MISMATCH;
  const auto decision = evaluate_firmware_package_trust(
      parsed, trust, root_key_id, root_public_key, verifier);
  this->trust_decision_ = decision;
  if (!decision.accepted())
    return FirmwareUploadResult::TRUST_REJECTED;
  this->package_ = parsed;
  this->session_id_ = session_id;
  this->last_activity_ms_ = now_ms;
  this->received_size_ = 0;
  this->active_ = true;
  this->sink_started_ = false;
  return FirmwareUploadResult::OK;
}

FirmwareUploadResult FirmwareUploadSession::fail_(FirmwareUploadResult result,
                                                   FirmwareUploadSink &sink) {
  if (this->sink_started_)
    sink.abort_firmware_image();
  this->active_ = false;
  this->sink_started_ = false;
  return result;
}

FirmwareUploadResult FirmwareUploadSession::consume(
    uint32_t session_id, std::size_t index, const uint8_t *data,
    std::size_t size, bool final, uint32_t now_ms, FirmwareUploadSink &sink) {
  if (!this->active_)
    return FirmwareUploadResult::NOT_ACTIVE;
  if (session_id != this->session_id_)
    return FirmwareUploadResult::SESSION_MISMATCH;
  if (now_ms - this->last_activity_ms_ > TIMEOUT_MS)
    return this->fail_(FirmwareUploadResult::EXPIRED, sink);
  if (index != this->received_size_ || size == 0 || data == nullptr)
    return this->fail_(FirmwareUploadResult::OFFSET_MISMATCH, sink);
  const std::size_t expected_size = FirmwarePackageHeader::HEADER_SIZE +
                                    this->package_.manifest.image_size;
  if (size > expected_size - this->received_size_)
    return this->fail_(FirmwareUploadResult::SIZE_MISMATCH, sink);

  std::size_t consumed = 0;
  if (this->received_size_ < FirmwarePackageHeader::HEADER_SIZE && size != 0) {
    const std::size_t header_remaining =
        FirmwarePackageHeader::HEADER_SIZE - this->received_size_;
    const std::size_t compare_size = std::min(header_remaining, size);
    const uint8_t *expected_header =
        this->received_size_ < FirmwarePackageHeader::SIGNED_SIZE
            ? this->package_.signed_bytes.data()
            : this->package_.signature.data();
    const std::size_t expected_offset =
        this->received_size_ < FirmwarePackageHeader::SIGNED_SIZE
            ? this->received_size_
            : this->received_size_ - FirmwarePackageHeader::SIGNED_SIZE;
    // A chunk may cross from signed bytes into the signature.
    const std::size_t first_part = std::min(
        compare_size,
        this->received_size_ < FirmwarePackageHeader::SIGNED_SIZE
            ? FirmwarePackageHeader::SIGNED_SIZE - this->received_size_
            : compare_size);
    if (std::memcmp(data, expected_header + expected_offset, first_part) != 0 ||
        (first_part < compare_size &&
         std::memcmp(data + first_part, this->package_.signature.data(),
                     compare_size - first_part) != 0))
      return this->fail_(FirmwareUploadResult::HEADER_MISMATCH, sink);
    this->received_size_ += compare_size;
    consumed += compare_size;
    if (this->received_size_ == FirmwarePackageHeader::HEADER_SIZE) {
      if (!sink.begin_firmware_image(this->package_.manifest.image_size))
        return this->fail_(FirmwareUploadResult::SINK_BEGIN_FAILED, sink);
      this->sink_started_ = true;
    }
  }

  if (consumed < size) {
    if (!this->sink_started_ ||
        !sink.write_firmware_image(data + consumed, size - consumed))
      return this->fail_(FirmwareUploadResult::SINK_WRITE_FAILED, sink);
    this->received_size_ += size - consumed;
  }
  this->last_activity_ms_ = now_ms;
  if (!final)
    return FirmwareUploadResult::OK;
  return this->finish(session_id, this->received_size_, now_ms, sink);
}

FirmwareUploadResult FirmwareUploadSession::finish(
    uint32_t session_id, std::size_t index, uint32_t now_ms,
    FirmwareUploadSink &sink) {
  if (!this->active_)
    return FirmwareUploadResult::NOT_ACTIVE;
  if (session_id != this->session_id_)
    return FirmwareUploadResult::SESSION_MISMATCH;
  if (now_ms - this->last_activity_ms_ > TIMEOUT_MS)
    return this->fail_(FirmwareUploadResult::EXPIRED, sink);
  if (index != this->received_size_)
    return this->fail_(FirmwareUploadResult::OFFSET_MISMATCH, sink);
  const std::size_t expected_size = FirmwarePackageHeader::HEADER_SIZE +
                                    this->package_.manifest.image_size;
  if (this->received_size_ != expected_size || !this->sink_started_)
    return this->fail_(FirmwareUploadResult::SIZE_MISMATCH, sink);
  if (!sink.finish_firmware_image(this->package_, this->trust_decision_))
    return this->fail_(FirmwareUploadResult::SINK_FINISH_FAILED, sink);
  this->active_ = false;
  this->sink_started_ = false;
  return FirmwareUploadResult::COMPLETE;
}

bool FirmwareUploadSession::expire(uint32_t now_ms,
                                   FirmwareUploadSink &sink) {
  if (!this->active_ || now_ms - this->last_activity_ms_ <= TIMEOUT_MS)
    return false;
  this->fail_(FirmwareUploadResult::EXPIRED, sink);
  return true;
}

bool FirmwareUploadSession::cancel(uint32_t session_id,
                                   FirmwareUploadSink &sink) {
  if (!this->active_ || session_id != this->session_id_)
    return false;
  this->fail_(FirmwareUploadResult::NOT_ACTIVE, sink);
  return true;
}

const char *firmware_upload_result_to_string(FirmwareUploadResult result) {
  switch (result) {
    case FirmwareUploadResult::OK: return "ok";
    case FirmwareUploadResult::COMPLETE: return "complete";
    case FirmwareUploadResult::BUSY: return "busy";
    case FirmwareUploadResult::NOT_ACTIVE: return "not_active";
    case FirmwareUploadResult::SESSION_MISMATCH: return "session_mismatch";
    case FirmwareUploadResult::EXPIRED: return "expired";
    case FirmwareUploadResult::OFFSET_MISMATCH: return "offset_mismatch";
    case FirmwareUploadResult::HEADER_MISMATCH: return "header_mismatch";
    case FirmwareUploadResult::SIZE_MISMATCH: return "size_mismatch";
    case FirmwareUploadResult::TRUST_REJECTED: return "trust_rejected";
    case FirmwareUploadResult::SINK_BEGIN_FAILED: return "sink_begin_failed";
    case FirmwareUploadResult::SINK_WRITE_FAILED: return "sink_write_failed";
    case FirmwareUploadResult::SINK_FINISH_FAILED: return "sink_finish_failed";
  }
  return "unknown";
}

}  // namespace radar_api_server
}  // namespace esphome
