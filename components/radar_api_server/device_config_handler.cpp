#include "device_config_handler.h"

#include "http_response.h"

namespace esphome {
namespace radar_api_server {

bool DeviceConfigHandler::can_handle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

  if (request->method() == HTTP_GET)
    return url == "/api/config/status" || url == "/api/config";
  if (request->method() == HTTP_POST)
    return url == "/api/config";
  return false;
}

bool DeviceConfigHandler::handle(AsyncWebServerRequest *request) {
  if (!this->can_handle(request))
    return false;

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

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

  if (request->method() == HTTP_POST && url == "/api/config") {
    this->handle_post_config_(request);
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
  std::string data;
  if (!this->storage_->read_payload(RadarPayloadTarget::DEVICE_CONFIG, &data)) {
    http_response::send_error(request, 404, "config_not_found");
    return;
  }

  auto *response = request->beginResponse(200, "application/json", data);
  request->send(response);
}

void DeviceConfigHandler::handle_post_config_(AsyncWebServerRequest *request) {
  if (!request->hasArg("data")) {
    http_response::send_error(request, 400, "missing_data");
    return;
  }

  const std::string data = request->arg("data");
  if (!this->storage_->write_payload(RadarPayloadTarget::DEVICE_CONFIG, reinterpret_cast<const uint8_t *>(data.data()),
                                     data.size())) {
    http_response::send_error(request, 500, "config_write_failed");
    return;
  }

  this->cache_->update(data);
  http_response::send_json(request, 200, R"({"ok":true})");
}

}  // namespace radar_api_server
}  // namespace esphome
