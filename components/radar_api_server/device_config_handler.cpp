#include "device_config_handler.h"

#include "http_response.h"
#include "strict_parse.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <esp_http_server.h>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace esphome {
namespace radar_api_server {

namespace {

static const char *const TAG = "device_config_handler";
static constexpr uint32_t DEVICE_CONFIG_STREAM_CHUNK_SIZE = 512;

}  // namespace

bool DeviceConfigHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET)
    return url == "/api/config/status" || url == "/api/config";
  if (request->method() == HTTP_POST)
    return url == "/api/config/upload/start" || url == "/api/config/upload/chunk" ||
           url == "/api/config/upload/commit";
  return false;
}

bool DeviceConfigHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (request->method() == HTTP_GET) {
    if (url == "/api/config/status") {
      this->handle_status_(request);
      return true;
    }
    if (url == "/api/config") {
      this->handle_get_config_(request);
      return true;
    }
  }

  if (request->method() == HTTP_POST && url == "/api/config/upload/start") {
    this->handle_upload_start_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/config/upload/chunk") {
    this->handle_upload_chunk_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/config/upload/commit") {
    this->handle_upload_commit_(request);
    return true;
  }

  return false;
}

void DeviceConfigHandler::handle_status_(AsyncWebServerRequest *request) {
  const auto status = this->storage_->status();
  const auto &header = status.header;

  auto *stream = request->beginResponseStream("application/json");
  stream->printf(R"({"ok":%s,"hasConfig":%s,"bytes":%u,"maxBytes":%u,"storage":"raw-partition"})",
                 status.ok ? "true" : "false", header.reserved[0] > 0 ? "true" : "false",
                 static_cast<unsigned>(header.reserved[0]),
                 static_cast<unsigned>(this->storage_->payload_max_size(RadarPayloadTarget::DEVICE_CONFIG)));
  request->send(stream);
}

void DeviceConfigHandler::handle_get_config_(AsyncWebServerRequest *request) {
  uint32_t payload_offset = 0;
  uint32_t payload_size = 0;
  if (!this->storage_->payload_info(RadarPayloadTarget::DEVICE_CONFIG, &payload_offset, &payload_size)) {
    http_response::send_error_info(request, 404, "config_not_found", "config_not_found", "error",
                                   R"({"target":"device_config"})");
    return;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[DEVICE_CONFIG_STREAM_CHUNK_SIZE]);
  if (!buffer) {
    ESP_LOGE(TAG, "config stream buffer allocation failed size=%u",
             static_cast<unsigned>(DEVICE_CONFIG_STREAM_CHUNK_SIZE));
    http_response::send_error_info(request, 500, "config_read_failed", "internal_error", "error",
                                   R"({"target":"device_config","where":"stream_buffer"})");
    return;
  }

  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");

  uint32_t sent = 0;
  while (sent < payload_size) {
    const uint32_t chunk_size = std::min<uint32_t>(DEVICE_CONFIG_STREAM_CHUNK_SIZE, payload_size - sent);
    if (!this->storage_->read_storage_range(payload_offset + sent, buffer.get(), chunk_size)) {
      ESP_LOGE(TAG, "config stream read failed offset=%u size=%u",
               static_cast<unsigned>(payload_offset + sent), static_cast<unsigned>(chunk_size));
      httpd_resp_send_chunk(raw_request, nullptr, 0);
      return;
    }
    if (httpd_resp_send_chunk(raw_request, reinterpret_cast<const char *>(buffer.get()), chunk_size) != ESP_OK) {
      ESP_LOGE(TAG, "config stream send failed sent=%u size=%u", static_cast<unsigned>(sent),
               static_cast<unsigned>(payload_size));
      return;
    }
    sent += chunk_size;
  }

  if (httpd_resp_send_chunk(raw_request, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "config stream final chunk failed size=%u", static_cast<unsigned>(payload_size));
    return;
  }
}

void DeviceConfigHandler::handle_upload_start_(AsyncWebServerRequest *request) {
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error",
                                   R"({"field":"session"})");
    return;
  }
  if (!request->hasArg("size")) {
    http_response::send_error_info(request, 400, "missing_size", "invalid_request", "error",
                                   R"({"field":"size"})");
    return;
  }

  uint32_t size = 0;
  const uint32_t max_size = this->storage_->payload_max_size(RadarPayloadTarget::DEVICE_CONFIG);
  if (!parse_uint32_strict(request->arg("size"), &size, false, false)) {
    http_response::send_error_info(request, 400, "invalid_size",
                                   "invalid_request", "error",
                                   R"({"field":"size"})");
    return;
  }
  if (size > max_size) {
    std::string detail = R"({"target":"device_config","maxBytes":)";
    detail += std::to_string(max_size);
    detail += R"(,"receivedBytes":)";
    detail += std::to_string(size);
    detail += "}";
    http_response::send_error_info(request, 413, "payload_too_large", "upload_payload_too_large", "error",
                                   detail.c_str());
    return;
  }

  if (!this->storage_->start_upload(RadarPayloadTarget::DEVICE_CONFIG, size, session_id)) {
    http_response::send_error_info(request, 500, "erase_failed", "upload_storage_failed", "error",
                                   R"({"target":"device_config","where":"start_upload"})");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void DeviceConfigHandler::handle_upload_chunk_(AsyncWebServerRequest *request) {
  if (!request->hasArg("offset") || !request->hasArg("data")) {
    http_response::send_error_info(request, 400, "missing_chunk", "invalid_request", "error",
                                   R"({"field":"offset,data"})");
    return;
  }
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error",
                                   R"({"field":"session"})");
    return;
  }

  uint32_t offset = 0;
  if (!parse_uint32_strict(request->arg("offset"), &offset)) {
    http_response::send_error_info(request, 400, "invalid_offset",
                                   "invalid_request", "error",
                                   R"({"field":"offset"})");
    return;
  }
  std::string decoded;
  if (!this->decode_hex_(request->arg("data"), &decoded)) {
    http_response::send_error_info(request, 400, "invalid_hex", "invalid_request", "error",
                                   R"({"field":"data","reason":"invalid_hex"})");
    return;
  }

  if (!this->storage_->write_upload_chunk(RadarPayloadTarget::DEVICE_CONFIG, offset,
                                          reinterpret_cast<const uint8_t *>(decoded.data()), decoded.size(),
                                          session_id)) {
    std::string detail = R"({"target":"device_config","where":"write_upload_chunk","offset":)";
    detail += std::to_string(offset);
    detail += R"(,"size":)";
    detail += std::to_string(decoded.size());
    detail += "}";
    http_response::send_error_info(request, 500, "chunk_write_failed", "upload_storage_failed", "error",
                                   detail.c_str());
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void DeviceConfigHandler::handle_upload_commit_(AsyncWebServerRequest *request) {
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error",
                                   R"({"field":"session"})");
    return;
  }

  if (!this->storage_->commit_upload(RadarPayloadTarget::DEVICE_CONFIG, session_id)) {
    http_response::send_error_info(request, 409, "upload_incomplete", "upload_incomplete", "error",
                                   R"({"target":"device_config"})");
    return;
  }

  std::string data;
  if (!this->storage_->read_payload(RadarPayloadTarget::DEVICE_CONFIG, &data)) {
    http_response::send_error_info(request, 500, "config_read_failed", "config_read_failed", "error",
                                   R"({"target":"device_config","where":"commit_reload"})");
    return;
  }

  this->cache_->update(std::move(data));
  http_response::send_json(request, 200, R"({"ok":true})");
}

bool DeviceConfigHandler::parse_upload_session_(AsyncWebServerRequest *request, uint32_t *session_id) const {
  if (!request->hasArg("session"))
    return false;
  uint32_t parsed = 0;
  if (!parse_uint32_strict(request->arg("session"), &parsed, true, false))
    return false;
  *session_id = parsed;
  return true;
}

bool DeviceConfigHandler::decode_hex_(const std::string &hex, std::string *out) const {
  if ((hex.size() % 2) != 0)
    return false;

  out->clear();
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    char pair[3] = {hex[i], hex[i + 1], '\0'};
    if (!std::isxdigit(static_cast<unsigned char>(pair[0])) ||
        !std::isxdigit(static_cast<unsigned char>(pair[1])))
      return false;
    out->push_back(static_cast<char>(std::strtoul(pair, nullptr, 16)));
  }
  return true;
}

}  // namespace radar_api_server
}  // namespace esphome
