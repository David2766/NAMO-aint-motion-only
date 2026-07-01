#include "control_handler.h"

#include "http_response.h"

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
    return url == "/api/control/status";
  if (request->method() == HTTP_POST)
    return url == "/api/control/status-led" || url == "/api/control/led-duration" ||
           url == "/api/control/environment-correction" || url == "/api/control/temperature-offset" ||
           url == "/api/control/humidity-offset";
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

  return false;
}

void ControlHandler::handle_status_(AsyncWebServerRequest *request) {
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      R"({"ok":true,"statusLedEnabled":%s,"statusLedKnown":%s,"ledBlinkDuration":%.1f,"ledBlinkDurationKnown":%s,"environmentCorrectionEnabled":%s,"environmentCorrectionKnown":%s,"temperatureOffset":%.1f,"temperatureOffsetKnown":%s,"humidityOffset":%.1f,"humidityOffsetKnown":%s})",
      this->control_state_->status_led_enabled ? "true" : "false",
      this->control_state_->has_status_led_enabled ? "true" : "false", this->control_state_->led_blink_duration,
      this->control_state_->has_led_blink_duration ? "true" : "false",
      this->control_state_->environment_correction_enabled ? "true" : "false",
      this->control_state_->has_environment_correction_enabled ? "true" : "false",
      this->control_state_->temperature_offset,
      this->control_state_->has_temperature_offset ? "true" : "false",
      this->control_state_->humidity_offset,
      this->control_state_->has_humidity_offset ? "true" : "false");
  request->send(stream);
}

void ControlHandler::handle_status_led_(AsyncWebServerRequest *request) {
  std::string body;
  bool enabled = false;
  if (!body_arg(request, &body) || !read_bool_field(body, "enabled", &enabled)) {
    http_response::send_error(request, 400, "invalid_enabled");
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
    http_response::send_error(request, 400, "invalid_seconds");
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
    http_response::send_error(request, 400, "invalid_enabled");
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
    http_response::send_error(request, 400, "invalid_value");
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
    http_response::send_error(request, 400, "invalid_value");
    return;
  }

  this->control_state_->requested_humidity_offset = clamp_humidity_offset(value);
  this->control_state_->pending_humidity_offset = true;
  this->control_state_->humidity_offset = this->control_state_->requested_humidity_offset;
  this->control_state_->has_humidity_offset = true;
  http_response::send_json(request, 200, R"({"ok":true})");
}

}  // namespace radar_api_server
}  // namespace esphome
