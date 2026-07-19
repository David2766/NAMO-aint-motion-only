#include "control_handler.h"

#include "http_response.h"
#include "timezone_catalog.h"
#include "esphome/core/hal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace esphome {
namespace radar_api_server {

namespace {

bool body_arg(AsyncWebServerRequest *request, std::string *out) {
  if (!request->hasArg("data"))
    return false;
  *out = request->arg("data");
  return true;
}

bool read_bool_field(const std::string &body, const char *field, bool *out) {
  const std::string key = std::string("\"") + field + "\"";
  const size_t key_pos = body.find(key);
  if (key_pos == std::string::npos)
    return false;
  const size_t colon = body.find(':', key_pos + key.size());
  if (colon == std::string::npos)
    return false;
  size_t pos = colon + 1;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n'))
    pos++;
  if (body.compare(pos, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (body.compare(pos, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool read_float_field(const std::string &body, const char *field, float *out) {
  const std::string key = std::string("\"") + field + "\"";
  const size_t key_pos = body.find(key);
  if (key_pos == std::string::npos)
    return false;
  const size_t colon = body.find(':', key_pos + key.size());
  if (colon == std::string::npos)
    return false;
  const char *start = body.c_str() + colon + 1;
  char *end = nullptr;
  const float value = strtof(start, &end);
  if (end == start)
    return false;
  *out = value;
  return true;
}

bool read_int_field(const std::string &body, const char *field, int *out) {
  const std::string key = std::string("\"") + field + "\"";
  const size_t key_pos = body.find(key);
  if (key_pos == std::string::npos)
    return false;
  const size_t colon = body.find(':', key_pos + key.size());
  if (colon == std::string::npos)
    return false;
  const char *start = body.c_str() + colon + 1;
  char *end = nullptr;
  const long value = strtol(start, &end, 10);
  if (end == start)
    return false;
  *out = static_cast<int>(value);
  return true;
}

bool read_string_field(const std::string &body, const char *field, std::string *out) {
  const std::string key = std::string("\"") + field + "\"";
  const size_t key_pos = body.find(key);
  if (key_pos == std::string::npos)
    return false;
  const size_t colon = body.find(':', key_pos + key.size());
  if (colon == std::string::npos)
    return false;
  const size_t quote = body.find('"', colon + 1);
  if (quote == std::string::npos)
    return false;
  const size_t end = body.find('"', quote + 1);
  if (end == std::string::npos || end == quote + 1 || end - quote - 1 > 64)
    return false;
  const std::string value = body.substr(quote + 1, end - quote - 1);
  if (value.find('\\') != std::string::npos)
    return false;
  *out = value;
  return true;
}

float clamp_led_duration(float value) {
  if (value < 0.0f)
    return 0.0f;
  if (value > 300.0f)
    return 300.0f;
  return value;
}

float clamp_temperature_offset(float value) {
  if (value < -2.0f)
    return -2.0f;
  if (value > 2.0f)
    return 2.0f;
  return value;
}

float clamp_humidity_offset(float value) {
  if (value < -5.0f)
    return -5.0f;
  if (value > 5.0f)
    return 5.0f;
  return value;
}

}  // namespace

bool ControlHandler::can_handle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

  if (request->method() == HTTP_GET)
    return url == "/api/control/status" || url == "/api/control/static-radar-tuning";
  if (request->method() == HTTP_POST)
    return url == "/api/control/status-led" || url == "/api/control/led-duration" ||
           url == "/api/control/environment-correction" || url == "/api/control/temperature-offset" ||
           url == "/api/control/humidity-offset" || url == "/api/control/timezone" ||
           url == "/api/control/static-radar-tuning/session" ||
           url == "/api/control/static-radar-tuning/gate";
  return false;
}

bool ControlHandler::handle(AsyncWebServerRequest *request) {
  if (!this->can_handle(request))
    return false;

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

  if (request->method() == HTTP_GET && url == "/api/control/status") {
    this->handle_status_(request);
    return true;
  }

  if (request->method() == HTTP_GET && url == "/api/control/static-radar-tuning") {
    this->handle_static_radar_tuning_status_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/status-led") {
    this->handle_status_led_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/led-duration") {
    this->handle_led_duration_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/environment-correction") {
    this->handle_environment_correction_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/temperature-offset") {
    this->handle_temperature_offset_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/humidity-offset") {
    this->handle_humidity_offset_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/timezone") {
    this->handle_timezone_(request);
    return true;
  }


  if (request->method() == HTTP_POST && url == "/api/control/static-radar-tuning/session") {
    this->handle_static_radar_tuning_session_(request);
    return true;
  }

  if (request->method() == HTTP_POST && url == "/api/control/static-radar-tuning/gate") {
    this->handle_static_radar_tuning_gate_(request);
    return true;
  }

  return false;
}

void ControlHandler::handle_status_(AsyncWebServerRequest *request) {
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      R"({"ok":true,"statusLedEnabled":%s,"statusLedKnown":%s,"ledBlinkDuration":%.1f,"ledBlinkDurationKnown":%s,"environmentCorrectionEnabled":%s,"environmentCorrectionKnown":%s,"temperatureOffset":%.1f,"temperatureOffsetKnown":%s,"humidityOffset":%.1f,"humidityOffsetKnown":%s,"timezone":"%s","timezoneKnown":%s,"timezoneApplyPending":%s})",
      this->control_state_->status_led_enabled ? "true" : "false",
      this->control_state_->has_status_led_enabled ? "true" : "false", this->control_state_->led_blink_duration,
      this->control_state_->has_led_blink_duration ? "true" : "false",
      this->control_state_->environment_correction_enabled ? "true" : "false",
      this->control_state_->has_environment_correction_enabled ? "true" : "false",
      this->control_state_->temperature_offset,
      this->control_state_->has_temperature_offset ? "true" : "false",
      this->control_state_->humidity_offset,
      this->control_state_->has_humidity_offset ? "true" : "false", this->control_state_->timezone.c_str(),
      this->control_state_->has_timezone ? "true" : "false",
      this->control_state_->pending_timezone ? "true" : "false");
  request->send(stream);
}

void ControlHandler::handle_status_led_(AsyncWebServerRequest *request) {
  std::string body;
  bool enabled = false;
  if (!body_arg(request, &body) || !read_bool_field(body, "enabled", &enabled)) {
    http_response::send_error_info(request, 400, "invalid_enabled", "invalid_request", "error",
                                   R"({"field":"enabled","target":"status_led"})");
    return;
  }

  this->control_state_->requested_status_led_enabled = enabled;
  this->control_state_->pending_status_led_enabled = true;
  this->control_state_->status_led_enabled = enabled;
  this->control_state_->has_status_led_enabled = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_led_duration_(AsyncWebServerRequest *request) {
  std::string body;
  float seconds = 0.0f;
  if (!body_arg(request, &body) || !read_float_field(body, "seconds", &seconds)) {
    http_response::send_error_info(request, 400, "invalid_seconds", "invalid_request", "error",
                                   R"({"field":"seconds","target":"led_duration"})");
    return;
  }

  this->control_state_->requested_led_blink_duration = clamp_led_duration(seconds);
  this->control_state_->pending_led_blink_duration = true;
  this->control_state_->led_blink_duration = this->control_state_->requested_led_blink_duration;
  this->control_state_->has_led_blink_duration = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_environment_correction_(AsyncWebServerRequest *request) {
  std::string body;
  bool enabled = false;
  if (!body_arg(request, &body) || !read_bool_field(body, "enabled", &enabled)) {
    http_response::send_error_info(request, 400, "invalid_enabled", "invalid_request", "error",
                                   R"({"field":"enabled","target":"environment_correction"})");
    return;
  }

  this->control_state_->requested_environment_correction_enabled = enabled;
  this->control_state_->pending_environment_correction_enabled = true;
  this->control_state_->environment_correction_enabled = enabled;
  this->control_state_->has_environment_correction_enabled = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_temperature_offset_(AsyncWebServerRequest *request) {
  std::string body;
  float value = 0.0f;
  if (!body_arg(request, &body) || !read_float_field(body, "value", &value)) {
    http_response::send_error_info(request, 400, "invalid_value", "invalid_request", "error",
                                   R"({"field":"value","target":"temperature_offset"})");
    return;
  }

  this->control_state_->requested_temperature_offset = clamp_temperature_offset(value);
  this->control_state_->pending_temperature_offset = true;
  this->control_state_->temperature_offset = this->control_state_->requested_temperature_offset;
  this->control_state_->has_temperature_offset = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_humidity_offset_(AsyncWebServerRequest *request) {
  std::string body;
  float value = 0.0f;
  if (!body_arg(request, &body) || !read_float_field(body, "value", &value)) {
    http_response::send_error_info(request, 400, "invalid_value", "invalid_request", "error",
                                   R"({"field":"value","target":"humidity_offset"})");
    return;
  }

  this->control_state_->requested_humidity_offset = clamp_humidity_offset(value);
  this->control_state_->pending_humidity_offset = true;
  this->control_state_->humidity_offset = this->control_state_->requested_humidity_offset;
  this->control_state_->has_humidity_offset = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_timezone_(AsyncWebServerRequest *request) {
  std::string body;
  std::string timezone;
  if (!body_arg(request, &body) || !read_string_field(body, "timezone", &timezone) ||
      !is_supported_timezone(timezone.c_str())) {
    http_response::send_error_info(request, 400, "invalid_timezone", "invalid_request", "error",
                                   R"({"field":"timezone","target":"timezone"})");
    return;
  }

  if (this->control_state_->has_timezone && this->control_state_->timezone == timezone) {
    http_response::send_json(request, 200, R"({"ok":true,"changed":false})");
    return;
  }

  this->control_state_->requested_timezone = timezone;
  this->control_state_->pending_timezone = true;
  this->control_state_->timezone = timezone;
  this->control_state_->has_timezone = true;
  // ESPHome's IDF web adapter maps unsupported response codes, including 202,
  // to 500. The pending state remains observable through the status endpoint.
  http_response::send_json(request, 200, R"({"ok":true,"changed":true,"todayStatsReset":true})");
}

void ControlHandler::handle_static_radar_tuning_status_(AsyncWebServerRequest *request) {
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(R"({"ok":true,"available":%s,"active":%s,"resolutionMm":%u,"gates":[)",
                 this->control_state_->static_radar_available ? "true" : "false",
                 this->control_state_->static_radar_tuning_active ? "true" : "false",
                 static_cast<unsigned>(this->control_state_->static_radar_gate_resolution_mm));

  if (this->control_state_->static_radar_tuning_active) {
    for (size_t gate = 0; gate < ControlState::STATIC_RADAR_GATE_COUNT; gate++) {
      const auto &value = this->control_state_->static_radar_gates[gate];
      const float effective_threshold = std::min(value.move_threshold, value.still_threshold);
      const int sensitivity = static_cast<int>(std::round(100.0f - effective_threshold));
      stream->printf(
          R"(%s{"gate":%u,"startMm":%u,"endMm":%u,"sensitivity":%d,"moveEnergy":%.0f,"stillEnergy":%.0f})",
          gate == 0 ? "" : ",", static_cast<unsigned>(gate),
          static_cast<unsigned>(gate * this->control_state_->static_radar_gate_resolution_mm),
          static_cast<unsigned>((gate + 1) * this->control_state_->static_radar_gate_resolution_mm),
          std::max(0, std::min(100, sensitivity)), value.move_energy, value.still_energy);
    }
  }

  stream->print("]}");
  request->send(stream);
}

void ControlHandler::handle_static_radar_tuning_session_(AsyncWebServerRequest *request) {
  std::string body;
  bool active = false;
  if (!body_arg(request, &body) || !read_bool_field(body, "active", &active)) {
    http_response::send_error_info(request, 400, "invalid_active", "invalid_request", "error",
                                   R"({"field":"active","target":"static_radar_tuning"})");
    return;
  }
  if (active && !this->control_state_->static_radar_available) {
    http_response::send_error_info(request, 409, "static_radar_unavailable", "static_radar_unavailable", "warning",
                                   R"({"target":"ld2410c"})");
    return;
  }

  const bool changed = this->control_state_->static_radar_tuning_requested_active != active;
  this->control_state_->static_radar_tuning_requested_active = active;
  this->control_state_->static_radar_tuning_keepalive_ms = active ? millis() : 0;
  this->control_state_->pending_static_radar_tuning_session =
      this->control_state_->pending_static_radar_tuning_session || changed;
  http_response::send_json(request, 200, R"({"ok":true})");
}

void ControlHandler::handle_static_radar_tuning_gate_(AsyncWebServerRequest *request) {
  std::string body;
  int gate = -1;
  int sensitivity = -1;
  if (!body_arg(request, &body) || !read_int_field(body, "gate", &gate) ||
      !read_int_field(body, "sensitivity", &sensitivity) || gate < 0 || gate >= 9 || sensitivity < 0 ||
      sensitivity > 100) {
    http_response::send_error_info(request, 400, "invalid_static_radar_gate", "invalid_request", "error",
                                   R"({"field":"gate,sensitivity","target":"static_radar_tuning"})");
    return;
  }
  if (!this->control_state_->static_radar_tuning_active) {
    http_response::send_error_info(request, 409, "static_radar_tuning_inactive", "static_radar_tuning_inactive",
                                   "warning", R"({"target":"ld2410c"})");
    return;
  }

  this->control_state_->requested_static_radar_gate = static_cast<uint8_t>(gate);
  this->control_state_->requested_static_radar_sensitivity = static_cast<uint8_t>(sensitivity);
  this->control_state_->pending_static_radar_gate = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

}  // namespace radar_api_server
}  // namespace esphome
