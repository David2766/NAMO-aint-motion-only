#include "diagnostic_handler.h"

#include "http_response.h"

#include <cstring>
#include <esp_http_server.h>

namespace esphome {
namespace radar_api_server {

bool DiagnosticHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() != HTTP_GET)
    return false;
  return url == "/api/diagnostics/events" || url == "/api/diagnostics/events.txt" ||
         url == "/api/diagnostics/replay.ndjson";
}

bool DiagnosticHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (url == "/api/diagnostics/replay.ndjson") {
    this->handle_replay_ndjson_(request);
  } else if (url == "/api/diagnostics/events.txt") {
    this->handle_events_text_(request);
  } else {
    this->handle_events_json_(request);
  }
  return true;
}

void DiagnosticHandler::handle_events_json_(AsyncWebServerRequest *request) {
  if (this->diagnostic_log_ == nullptr || !this->diagnostic_log_->available()) {
    http_response::send_error_info(request, 503, "debug_events_unavailable", "debug_events_unavailable", "warning",
                                   R"({"resource":"diagnostic_events"})");
    return;
  }

  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");

  char chunk[192];
  this->diagnostic_log_->format_json_header(chunk, sizeof(chunk));
  if (httpd_resp_send_chunk(raw_request, chunk, std::strlen(chunk)) != ESP_OK)
    return;

  std::string event_json;
  for (size_t i = 0; i < this->diagnostic_log_->count(); i++) {
    if (i > 0 && httpd_resp_send_chunk(raw_request, ",", 1) != ESP_OK)
      return;
    if (!this->diagnostic_log_->format_json_event(i, &event_json))
      continue;
    if (httpd_resp_send_chunk(raw_request, event_json.data(), event_json.size()) != ESP_OK)
      return;
  }

  this->diagnostic_log_->format_json_footer(chunk, sizeof(chunk));
  if (httpd_resp_send_chunk(raw_request, chunk, std::strlen(chunk)) != ESP_OK)
    return;
  httpd_resp_send_chunk(raw_request, nullptr, 0);
}

void DiagnosticHandler::handle_events_text_(AsyncWebServerRequest *request) {
  if (this->diagnostic_log_ == nullptr || !this->diagnostic_log_->available()) {
    http_response::send_error_info(request, 503, "debug_events_unavailable", "debug_events_unavailable", "warning",
                                   R"({"resource":"diagnostic_events"})");
    return;
  }

  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(raw_request, "Content-Disposition", "attachment; filename=\"presence-diagnostics.txt\"");

  char line[512];
  this->diagnostic_log_->format_text_header(line, sizeof(line));
  if (httpd_resp_send_chunk(raw_request, line, std::strlen(line)) != ESP_OK)
    return;

  for (size_t i = 0; i < this->diagnostic_log_->count(); i++) {
    if (!this->diagnostic_log_->format_text_event(i, line, sizeof(line)))
      continue;
    if (httpd_resp_send_chunk(raw_request, line, std::strlen(line)) != ESP_OK)
      return;
  }

  httpd_resp_send_chunk(raw_request, nullptr, 0);
}

void DiagnosticHandler::handle_replay_ndjson_(AsyncWebServerRequest *request) {
  if (this->presence_replay_log_ == nullptr || !this->presence_replay_log_->available()) {
    http_response::send_error_info(request, 503, "debug_replay_unavailable", "debug_replay_unavailable", "warning",
                                   R"({"resource":"presence_replay"})");
    return;
  }

  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, "application/x-ndjson; charset=utf-8");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(raw_request, "Content-Disposition", "attachment; filename=\"presence-replay.ndjson\"");

  char line[640];
  for (size_t i = 0; i < this->presence_replay_log_->count(); i++) {
    if (!this->presence_replay_log_->format_ndjson_sample(i, line, sizeof(line)))
      continue;
    if (httpd_resp_send_chunk(raw_request, line, std::strlen(line)) != ESP_OK)
      return;
  }

  httpd_resp_send_chunk(raw_request, nullptr, 0);
}

}  // namespace radar_api_server
}  // namespace esphome
