#include "http_response.h"

#include <string>

namespace esphome {
namespace radar_api_server {
namespace http_response {

void send_gzip_asset(AsyncWebServerRequest *request, const char *content_type, const uint8_t *data, size_t size) {
  auto *response = request->beginResponse(200, content_type, data, size);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void send_json(AsyncWebServerRequest *request, int code, const char *json) {
  request->send(code, "application/json", json);
}

void send_error(AsyncWebServerRequest *request, int code, const char *message) {
  std::string body = R"({"ok":false,"error":")";
  body += message;
  body += R"("})";
  auto *response = request->beginResponse(code, "application/json", body);
  request->send(response);
}

}  // namespace http_response
}  // namespace radar_api_server
}  // namespace esphome
