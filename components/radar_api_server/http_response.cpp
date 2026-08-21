#include "http_response.h"

#include <cstdio>
#include <string>

namespace esphome {
namespace radar_api_server {
namespace http_response {

namespace {

void append_json_string(std::string *out, const char *value) {
  out->push_back('"');
  const char *safe = value != nullptr ? value : "";
  for (const char *cursor = safe; *cursor != '\0'; ++cursor) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    if (ch == '"' || ch == '\\') {
      out->push_back('\\');
      out->push_back(static_cast<char>(ch));
    } else if (ch < 0x20) {
      char escaped[7]{};
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
      out->append(escaped);
    } else {
      out->push_back(static_cast<char>(ch));
    }
  }
  out->push_back('"');
}

}  // namespace

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
  std::string body = R"({"ok":false,"error":)";
  append_json_string(&body, message);
  body += "}";
  auto *response = request->beginResponse(code, "application/json", body);
  request->send(response);
}

void send_error_info(AsyncWebServerRequest *request, int http_code, const char *code, const char *severity) {
  send_error_info(request, http_code, code, severity, "{}");
}

void send_error_info(AsyncWebServerRequest *request, int http_code, const char *code, const char *severity,
                     const char *detail_json) {
  send_error_info(request, http_code, code, code, severity, detail_json);
}

void send_error_info(AsyncWebServerRequest *request, int http_code, const char *legacy_error, const char *info_code,
                     const char *severity, const char *detail_json) {
  std::string body = R"({"ok":false,"error":)";
  append_json_string(&body, legacy_error);
  body += R"(,"errorInfo":{"code":)";
  append_json_string(&body, info_code);
  body += R"(,"severity":)";
  append_json_string(&body, severity);
  body += R"(,"detail":)";
  body += detail_json != nullptr ? detail_json : "{}";
  body += R"(}})";
  auto *response = request->beginResponse(http_code, "application/json", body);
  request->send(response);
}

}  // namespace http_response
}  // namespace radar_api_server
}  // namespace esphome
