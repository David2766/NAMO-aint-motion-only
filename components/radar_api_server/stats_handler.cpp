#include "stats_handler.h"

#include "http_response.h"
#include "esphome/core/log.h"

#include <cctype>
#include <cstdlib>
#include <esp_http_server.h>
#include <string>

namespace esphome {
namespace radar_api_server {

static const char *const TAG = "stats_handler";

bool StatsHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET)
    return url == "/api/stats";
  if (request->method() == HTTP_POST)
    return url == "/api/stats/upload/start" || url == "/api/stats/upload/chunk" ||
           url == "/api/stats/upload/commit";
  return false;
}

bool StatsHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (request->method() == HTTP_GET) {
    this->handle_get_stats_(request);
    return true;
  }

  if (request->method() == HTTP_POST) {
    if (url == "/api/stats/upload/start") {
      this->handle_upload_start_(request);
      return true;
    }
    if (url == "/api/stats/upload/chunk") {
      this->handle_upload_chunk_(request);
      return true;
    }
    if (url == "/api/stats/upload/commit") {
      this->handle_upload_commit_(request);
      return true;
    }
  }

  return false;
}

void StatsHandler::handle_get_stats_(AsyncWebServerRequest *request) {
  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  if (!this->stats_store_->stream_json(raw_request))
    ESP_LOGW(TAG, "Statistics response stream failed");
}

void StatsHandler::handle_upload_start_(AsyncWebServerRequest *request) {
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

  const uint32_t size = std::strtoul(request->arg("size").c_str(), nullptr, 10);
  const uint32_t max_size = this->storage_->payload_max_size(RadarPayloadTarget::STATS);
  if (size == 0 || size > max_size) {
    std::string detail = R"({"target":"stats","maxBytes":)";
    detail += std::to_string(max_size);
    detail += R"(,"receivedBytes":)";
    detail += std::to_string(size);
    detail += "}";
    http_response::send_error_info(request, 413, "payload_too_large", "upload_payload_too_large", "error",
                                   detail.c_str());
    return;
  }

  if (!this->storage_->start_upload(RadarPayloadTarget::STATS, size, session_id)) {
    http_response::send_error_info(request, 500, "erase_failed", "upload_storage_failed", "error",
                                   R"({"target":"stats","where":"start_upload"})");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void StatsHandler::handle_upload_chunk_(AsyncWebServerRequest *request) {
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

  const uint32_t offset = std::strtoul(request->arg("offset").c_str(), nullptr, 10);
  std::string decoded;
  if (!this->decode_hex_(request->arg("data"), &decoded)) {
    http_response::send_error_info(request, 400, "invalid_hex", "invalid_request", "error",
                                   R"({"field":"data","reason":"invalid_hex"})");
    return;
  }

  if (!this->storage_->write_upload_chunk(RadarPayloadTarget::STATS, offset,
                                          reinterpret_cast<const uint8_t *>(decoded.data()), decoded.size(),
                                          session_id)) {
    std::string detail = R"({"target":"stats","where":"write_upload_chunk","offset":)";
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

void StatsHandler::handle_upload_commit_(AsyncWebServerRequest *request) {
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error",
                                   R"({"field":"session"})");
    return;
  }

  if (!this->storage_->commit_upload(RadarPayloadTarget::STATS, session_id)) {
    http_response::send_error_info(request, 409, "upload_incomplete", "upload_incomplete", "error",
                                   R"({"target":"stats"})");
    return;
  }

  this->stats_store_->load(this->storage_);
  http_response::send_json(request, 200, R"({"ok":true})");
}

bool StatsHandler::parse_upload_session_(AsyncWebServerRequest *request, uint32_t *session_id) const {
  if (!request->hasArg("session"))
    return false;
  const uint32_t parsed = std::strtoul(request->arg("session").c_str(), nullptr, 0);
  if (parsed == 0)
    return false;
  *session_id = parsed;
  return true;
}

bool StatsHandler::decode_hex_(const std::string &hex, std::string *out) const {
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
