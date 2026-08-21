#include "firmware_update_handler.h"

#include "firmware_trust_keys.h"
#include "http_response.h"
#include "radar_api_server.h"
#include "strict_parse.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
#include "esphome/components/sha256/sha256.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace esphome {
namespace radar_api_server {

namespace {

static const char *const TAG = "firmware_update";
constexpr uint32_t OTA_REBOOT_DELAY_MS = 100;

char hex_digit(uint8_t value) {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

void format_key_id(const std::array<uint8_t, 16> &key_id, char *output) {
  for (std::size_t index = 0; index < key_id.size(); ++index) {
    output[index * 2] = hex_digit(key_id[index] >> 4U);
    output[index * 2 + 1] = hex_digit(key_id[index] & 0x0fU);
  }
  output[32] = '\0';
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

}  // namespace

bool FirmwareUpdateHandler::setup() {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  this->trust_store_result_ = initialize_esp32_firmware_trust(
      &this->trust_snapshot_, &this->trust_backend_, &this->trust_store_);
  const bool ready =
      this->trust_store_result_ == FirmwareTrustStoreResult::LOADED ||
      this->trust_store_result_ == FirmwareTrustStoreResult::CREATED ||
      this->trust_store_result_ == FirmwareTrustStoreResult::UPDATED ||
      this->trust_store_result_ ==
          FirmwareTrustStoreResult::COMMIT_REPAIRED;
  if (ready) {
    ESP_LOGI(TAG,
             "Signed OTA trust ready implementation=NAMO_SIGNED_OTA_V1 "
             "revision=%u security_epoch=%u "
             "generation=%u",
             static_cast<unsigned>(this->trust_snapshot_.state.trust_revision),
             static_cast<unsigned>(
                 this->trust_snapshot_.state.minimum_security_epoch),
             static_cast<unsigned>(this->trust_snapshot_.generation));
  } else {
    ESP_LOGE(TAG, "Signed OTA disabled: trust store %s",
             firmware_trust_store_result_to_string(this->trust_store_result_));
  }
  return ready;
#else
  return false;
#endif
}

void FirmwareUpdateHandler::expire_stale_session_() {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (this->session_.expire(millis(), *this)) {
    this->upload_result_ = FirmwareUploadResult::EXPIRED;
    ESP_LOGW(TAG, "Signed OTA session expired");
  }
#endif
}

bool FirmwareUpdateHandler::can_handle(AsyncWebServerRequest *request,
                                        StringRef url) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  return (request->method() == HTTP_GET && url == "/api/firmware/trust") ||
         (request->method() == HTTP_POST &&
          (url == "/api/firmware/alpha-permission" ||
           url == "/api/firmware/update/start" ||
           url == "/api/firmware/update/cancel" ||
           url == "/api/firmware/update"));
#else
  (void) request;
  (void) url;
  return false;
#endif
}

bool FirmwareUpdateHandler::can_handle_upload(
    AsyncWebServerRequest *request, StringRef url) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  return request->method() == HTTP_POST &&
         url == "/api/firmware/update";
#else
  (void) request;
  (void) url;
  return false;
#endif
}

bool FirmwareUpdateHandler::handle(AsyncWebServerRequest *request,
                                    StringRef url) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (url == "/api/firmware/trust") {
    this->handle_status_(request);
    return true;
  }
  if (url == "/api/firmware/alpha-permission") {
    this->handle_alpha_permission_(request);
    return true;
  }
  if (url == "/api/firmware/update/start") {
    this->handle_start_(request);
    return true;
  }
  if (url == "/api/firmware/update/cancel") {
    this->handle_cancel_(request);
    return true;
  }
  if (url == "/api/firmware/update") {
    this->handle_upload_result_(request);
    return true;
  }
#else
  (void) request;
  (void) url;
#endif
  return false;
}

void FirmwareUpdateHandler::handle_status_(AsyncWebServerRequest *request) {
  this->expire_stale_session_();
  const uint32_t alpha_remaining_seconds =
      this->developer_install_gate_.remaining_seconds(millis());
  char release_id[33]{};
  char developer_id[33]{};
  if (this->trust_snapshot_.state.release_keys[0].occupied)
    format_key_id(this->trust_snapshot_.state.release_keys[0].key_id,
                  release_id);
  if (this->trust_snapshot_.state.developer_keys[0].occupied)
    format_key_id(this->trust_snapshot_.state.developer_keys[0].key_id,
                  developer_id);
  char response[640]{};
  std::snprintf(
      response, sizeof(response),
      "{\"ok\":true,\"data\":{\"mode\":\"signed\","
      "\"ready\":%s,\"trustRevision\":%u,\"securityEpoch\":%u,"
      "\"generation\":%u,\"releaseKeyId\":\"%s\","
      "\"developerKeyId\":\"%s\",\"sessionActive\":%s,"
      "\"alphaInstallSupported\":true,"
      "\"alphaInstallAllowed\":%s,"
      "\"alphaInstallRemainingSeconds\":%u}}",
      this->trust_snapshot_.generation != 0 ? "true" : "false",
      static_cast<unsigned>(this->trust_snapshot_.state.trust_revision),
      static_cast<unsigned>(
          this->trust_snapshot_.state.minimum_security_epoch),
      static_cast<unsigned>(this->trust_snapshot_.generation), release_id,
      developer_id, this->session_.active() ? "true" : "false",
      alpha_remaining_seconds != 0 ? "true" : "false",
      static_cast<unsigned>(alpha_remaining_seconds));
  http_response::send_json(request, 200, response);
}

void FirmwareUpdateHandler::handle_alpha_permission_(
    AsyncWebServerRequest *request) {
  if (!request->hasArg("enabled")) {
    http_response::send_error_info(
        request, 400, "alpha_firmware_permission_invalid", "warning");
    return;
  }
  const auto enabled_text = request->arg("enabled");
  const bool valid = enabled_text == "true" || enabled_text == "1" ||
                     enabled_text == "false" || enabled_text == "0";
  if (!valid) {
    http_response::send_error_info(
        request, 400, "alpha_firmware_permission_invalid", "warning");
    return;
  }
  const bool enabled = enabled_text == "true" || enabled_text == "1";
  this->developer_install_gate_.set_enabled(enabled, millis());
  ESP_LOGI(TAG, "Alpha firmware installation %s window=%us one_shot=true",
           enabled ? "allowed" : "disabled",
           enabled ? static_cast<unsigned>(
                         FirmwareDeveloperInstallGate::WINDOW_MS / 1000U)
                   : 0U);
  this->handle_status_(request);
}

bool FirmwareUpdateHandler::decode_header_(
    const std::string &hex,
    std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE> *header) const {
  if (header == nullptr || hex.size() != header->size() * 2)
    return false;
  for (std::size_t index = 0; index < header->size(); ++index) {
    const int high = hex_value(hex[index * 2]);
    const int low = hex_value(hex[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    (*header)[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

void FirmwareUpdateHandler::handle_start_(AsyncWebServerRequest *request) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  this->expire_stale_session_();
  if (this->trust_snapshot_.generation == 0 ||
      this->trust_backend_ == nullptr) {
    http_response::send_error_info(request, 503, "firmware_trust_unavailable",
                                   "error");
    return;
  }
  std::array<uint8_t, FirmwarePackageHeader::HEADER_SIZE> header{};
  if (!request->hasArg("header") ||
      !this->decode_header_(request->arg("header"), &header)) {
    http_response::send_error_info(request, 400, "firmware_header_invalid",
                                   "error");
    return;
  }
  uint32_t session_id = random_uint32();
  if (session_id == 0)
    session_id = 1;
  this->upload_success_ = false;
  this->upload_result_ = this->session_.start(
      header.data(), header.size(), session_id, millis(),
      this->trust_snapshot_.state, firmware_trust_keys::ROOT_KEY_ID,
      firmware_trust_keys::ROOT_PUBLIC_KEY,
      this->verifier_);
  if (this->upload_result_ != FirmwareUploadResult::OK) {
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "{\"trust\":\"%s\"}",
                  firmware_trust_result_to_string(
                      this->session_.trust_decision().result));
    http_response::send_error_info(
        request,
        this->upload_result_ == FirmwareUploadResult::BUSY ? 409 : 400,
        firmware_upload_result_to_string(this->upload_result_), "error",
        detail);
    return;
  }
  if (this->session_.package().manifest.signer_role ==
          FirmwareSignerRole::DEVELOPER &&
      !this->developer_install_gate_.allowed(millis())) {
    this->session_.cancel(session_id, *this);
    this->upload_result_ = FirmwareUploadResult::NOT_ACTIVE;
    ESP_LOGW(TAG,
             "Valid Developer-signed alpha firmware rejected: one-shot "
             "permission is not active");
    http_response::send_error_info(request, 403,
                                   "alpha_firmware_not_allowed", "warning");
    return;
  }
  char response[192]{};
  std::snprintf(
      response, sizeof(response),
      "{\"ok\":true,\"data\":{\"session\":%u,\"imageSize\":%u,"
      "\"packageSize\":%u,\"signerRole\":\"%s\"}}",
      static_cast<unsigned>(session_id),
      static_cast<unsigned>(this->session_.image_size()),
      static_cast<unsigned>(FirmwarePackageHeader::HEADER_SIZE +
                            this->session_.image_size()),
      this->session_.package().manifest.signer_role ==
              FirmwareSignerRole::RELEASE
          ? "release"
          : this->session_.package().manifest.signer_role ==
                    FirmwareSignerRole::DEVELOPER
                ? "developer"
                : "root_recovery");
  http_response::send_json(request, 200, response);
#else
  (void) request;
#endif
}

void FirmwareUpdateHandler::handle_cancel_(AsyncWebServerRequest *request) {
  this->expire_stale_session_();
  uint32_t session_id = 0;
  if (!request->hasArg("session") ||
      !parse_uint32_strict(request->arg("session"), &session_id, true,
                           false) ||
      !this->session_.cancel(session_id, *this)) {
    http_response::send_error_info(request, 409, "session_mismatch",
                                   "warning");
    return;
  }
  this->upload_result_ = FirmwareUploadResult::NOT_ACTIVE;
  this->upload_success_ = false;
  http_response::send_json(request, 200, "{\"ok\":true}");
}

void FirmwareUpdateHandler::handle_upload_result_(
    AsyncWebServerRequest *request) {
  const FirmwareUploadResult result = this->upload_result_;
  const bool success = this->upload_success_ &&
                       result == FirmwareUploadResult::COMPLETE;
  this->upload_result_ = FirmwareUploadResult::NOT_ACTIVE;
  this->upload_success_ = false;
  if (success) {
    auto *response = request->beginResponse(
        200, "application/json",
        "{\"ok\":true,\"status\":{\"code\":\"ota_reboot_waiting\","
        "\"severity\":\"info\",\"detail\":{\"retryAfterMs\":100}}}");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
    ESP_LOGI(TAG, "OTA update successful; reboot scheduled");
    this->server_->reboot_after(OTA_REBOOT_DELAY_MS);
    return;
  }
  const int status =
      result == FirmwareUploadResult::SESSION_MISMATCH ? 409 : 400;
  http_response::send_error_info(
      request, status,
      firmware_upload_result_to_string(result), "error");
}

void FirmwareUpdateHandler::handle_upload(
    AsyncWebServerRequest *request, StringRef url,
    const std::string &filename,
    std::size_t index, uint8_t *data, std::size_t size, bool final) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (!can_handle_upload(request, url))
    return;
  (void) filename;
  const auto marker = classify_firmware_multipart_marker(
      index, data, size, final);
  if (marker == FirmwareMultipartMarker::START) {
    this->upload_result_ = FirmwareUploadResult::OK;
    this->upload_success_ = false;
    return;
  }
  if (this->upload_result_ != FirmwareUploadResult::OK)
    return;
  uint32_t session_id = 0;
  if (!request->hasArg("session") ||
      !parse_uint32_strict(request->arg("session"), &session_id, true,
                           false)) {
    this->upload_result_ = FirmwareUploadResult::SESSION_MISMATCH;
    return;
  }
  const std::size_t expected_offset = this->session_.received_size();
  this->upload_result_ = marker == FirmwareMultipartMarker::END
                             ? this->session_.finish(session_id, index,
                                                     millis(), *this)
                             : this->session_.consume(
                                   session_id, index, data, size, final,
                                   millis(), *this);
  this->upload_success_ =
      this->upload_result_ == FirmwareUploadResult::COMPLETE;
  if (this->upload_result_ != FirmwareUploadResult::OK &&
      this->upload_result_ != FirmwareUploadResult::COMPLETE) {
    ESP_LOGE(TAG,
             "Signed OTA upload failed: %s index=%u expected=%u size=%u "
             "final=%s",
             firmware_upload_result_to_string(this->upload_result_),
             static_cast<unsigned>(index),
             static_cast<unsigned>(expected_offset),
             static_cast<unsigned>(size), final ? "true" : "false");
  }
#else
  (void) request;
  (void) url;
  (void) filename;
  (void) index;
  (void) data;
  (void) size;
  (void) final;
#endif
}

bool FirmwareUpdateHandler::begin_firmware_image(uint32_t image_size) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  this->abort_firmware_image();
  this->update_partition_ = esp_ota_get_next_update_partition(nullptr);
  this->ota_backend_ = ota::make_ota_backend();
  if (this->update_partition_ == nullptr || !this->ota_backend_)
    return false;
  const auto result = this->ota_backend_->begin(image_size);
  if (result != ota::OTA_RESPONSE_OK) {
    this->ota_backend_.reset();
    this->update_partition_ = nullptr;
    return false;
  }
  ESP_LOGI(TAG, "Signed OTA image accepted for flash size=%u",
           static_cast<unsigned>(image_size));
  return true;
#else
  (void) image_size;
  return false;
#endif
}

bool FirmwareUpdateHandler::write_firmware_image(const uint8_t *data,
                                                  std::size_t size) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  return this->ota_backend_ != nullptr && data != nullptr &&
         this->ota_backend_->write(const_cast<uint8_t *>(data), size) ==
             ota::OTA_RESPONSE_OK;
#else
  (void) data;
  (void) size;
  return false;
#endif
}

bool FirmwareUpdateHandler::finish_firmware_image(
    const FirmwarePackageHeader &package,
    const FirmwareTrustDecision &trust) {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (!this->ota_backend_ || this->update_partition_ == nullptr)
    return false;
  sha256::SHA256 hasher;
  hasher.init();
  std::size_t offset = 0;
  while (offset < package.manifest.image_size) {
    const std::size_t chunk = std::min(
        this->verify_buffer_.size(),
        static_cast<std::size_t>(package.manifest.image_size) - offset);
    if (esp_partition_read(this->update_partition_, offset,
                           this->verify_buffer_.data(), chunk) != ESP_OK) {
      this->abort_firmware_image();
      return false;
    }
    hasher.add(this->verify_buffer_.data(), chunk);
    offset += chunk;
  }
  hasher.calculate();
  if (!hasher.equals_bytes(package.manifest.image_sha256.data())) {
    ESP_LOGE(TAG, "Signed OTA flash digest mismatch");
    this->abort_firmware_image();
    return false;
  }

  FirmwareTrustStagedUpdate staged_trust{};
  if (trust.trust_update_required) {
    const auto result = this->trust_store_.stage_update(
        *this->trust_backend_, trust.next_trust, this->trust_snapshot_,
        &staged_trust);
    if (result != FirmwareTrustStoreResult::STAGED) {
      this->trust_store_result_ = result;
      ESP_LOGE(TAG, "Trust rotation staging failed: %s",
               firmware_trust_store_result_to_string(result));
      this->abort_firmware_image();
      return false;
    }
    ESP_LOGI(TAG, "Trust rotation staged revision=%u generation=%u",
             static_cast<unsigned>(staged_trust.trust_revision),
             static_cast<unsigned>(staged_trust.generation));
  }

  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  if (this->ota_backend_->end() != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "Signed OTA image activation failed; staged trust ignored");
    this->ota_backend_.reset();
    this->update_partition_ = nullptr;
    return false;
  }
  this->ota_backend_.reset();
  this->update_partition_ = nullptr;

  if (trust.trust_update_required) {
    const auto result = this->trust_store_.commit_staged(
        *this->trust_backend_, staged_trust, &this->trust_snapshot_);
    if (result != FirmwareTrustStoreResult::UPDATED) {
      this->trust_store_result_ = result;
      const esp_err_t rollback_result =
          running_partition != nullptr
              ? esp_ota_set_boot_partition(running_partition)
              : ESP_ERR_INVALID_STATE;
      ESP_LOGE(TAG,
               "Trust rotation commit failed: %s boot_restore=%s",
               firmware_trust_store_result_to_string(result),
               esp_err_to_name(rollback_result));
      return false;
    }
    this->trust_store_result_ = result;
    ESP_LOGI(TAG, "Trust rotation committed revision=%u generation=%u",
             static_cast<unsigned>(
                 this->trust_snapshot_.state.trust_revision),
             static_cast<unsigned>(this->trust_snapshot_.generation));
  }

  ESP_LOGI(TAG, "Signed OTA update verified successfully");
  if (package.manifest.signer_role == FirmwareSignerRole::DEVELOPER) {
    this->developer_install_gate_.consume();
    ESP_LOGI(TAG, "Alpha firmware one-shot installation permission consumed");
  }
  return true;
#else
  (void) package;
  (void) trust;
  return false;
#endif
}

void FirmwareUpdateHandler::abort_firmware_image() {
#if defined(USE_ESP32) && defined(NAMO_SIGNED_OTA)
  if (this->ota_backend_ != nullptr)
    this->ota_backend_->abort();
  this->ota_backend_.reset();
  this->update_partition_ = nullptr;
#endif
}

}  // namespace radar_api_server
}  // namespace esphome
