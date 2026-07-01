#include "stats_handler.h"

#include "http_response.h"

#include <cctype>
#include <cstdlib>

namespace esphome {
namespace radar_api_server {

bool StatsHandler::can_handle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

  if (request->method() == HTTP_GET)
    return url == "/api/stats";
  if (request->method() == HTTP_POST)
    return url == "/api/stats" || url == "/api/stats/upload/start" || url == "/api/stats/upload/chunk" ||
           url == "/api/stats/upload/commit";
  return false;
}

bool StatsHandler::handle(AsyncWebServerRequest *request) {
  if (!this->can_handle(request))
    return false;

  if (request->method() == HTTP_GET) {
    this->handle_get_stats_(request);
    return true;
  }

  if (request->method() == HTTP_POST) {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    auto url = request->url_to(url_buf);
    if (url == "/api/stats") {
      this->handle_post_stats_(request);
      return true;
    }
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
  const std::string data = this->stats_store_->current_json();

  auto *response = request->beginResponse(200, "application/json", data);
  request->send(response);
}

void StatsHandler::handle_post_stats_(AsyncWebServerRequest *request) {
  if (!request->hasArg("data")) {
    http_response::send_error(request, 400, "missing_data");
    return;
  }

  const std::string data = request->arg("data");
  if (!this->stats_store_->replace_json(data)) {
    http_response::send_error(request, 500, "stats_write_failed");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void StatsHandler::handle_upload_start_(AsyncWebServerRequest *request) {
  if (!request->hasArg("size")) {
    http_response::send_error(request, 400, "missing_size");
    return;
  }

  const uint32_t size = std::strtoul(request->arg("size").c_str(), nullptr, 10);
  if (size == 0 || size > this->storage_->payload_max_size(RadarPayloadTarget::STATS)) {
    http_response::send_error(request, 413, "payload_too_large");
    return;
  }

  if (!this->storage_->start_upload(RadarPayloadTarget::STATS, size)) {
    http_response::send_error(request, 500, "erase_failed");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void StatsHandler::handle_upload_chunk_(AsyncWebServerRequest *request) {
  if (!request->hasArg("offset") || !request->hasArg("data")) {
    http_response::send_error(request, 400, "missing_chunk");
    return;
  }

  const uint32_t offset = std::strtoul(request->arg("offset").c_str(), nullptr, 10);
  std::string decoded;
  if (!this->decode_hex_(request->arg("data"), &decoded)) {
    http_response::send_error(request, 400, "invalid_hex");
    return;
  }

  if (!this->storage_->write_upload_chunk(RadarPayloadTarget::STATS, offset,
                                          reinterpret_cast<const uint8_t *>(decoded.data()), decoded.size())) {
    http_response::send_error(request, 500, "chunk_write_failed");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void StatsHandler::handle_upload_commit_(AsyncWebServerRequest *request) {
  if (!this->storage_->commit_upload(RadarPayloadTarget::STATS)) {
    http_response::send_error(request, 409, "upload_incomplete");
    return;
  }

  this->stats_store_->load(this->storage_);
  http_response::send_json(request, 200, R"({"ok":true})");
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
