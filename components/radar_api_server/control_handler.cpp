#include "control_handler.h"

#include "http_response.h"
#include "timezone_catalog.h"
#include "esphome/core/hal.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

const char *skip_json_whitespace(const char *cursor) {
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n')
    cursor++;
  return cursor;
}

bool valid_json_value_end(const char *cursor) {
  cursor = skip_json_whitespace(cursor);
  return *cursor == ',' || *cursor == '}';
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
    if (!valid_json_value_end(body.c_str() + pos + 4))
      return false;
    *out = true;
    return true;
  }
  if (body.compare(pos, 5, "false") == 0) {
    if (!valid_json_value_end(body.c_str() + pos + 5))
      return false;
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
  const char *start = skip_json_whitespace(body.c_str() + colon + 1);
  if (*start == '+')
    return false;
  char *end = nullptr;
  errno = 0;
  const float value = strtof(start, &end);
  if (end == start || errno == ERANGE || !std::isfinite(value) ||
      !valid_json_value_end(end))
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
  const char *start = skip_json_whitespace(body.c_str() + colon + 1);
  if (*start == '+')
    return false;
  char *end = nullptr;
  errno = 0;
  const long value = strtol(start, &end, 10);
  if (end == start || errno == ERANGE ||
      value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max() ||
      !valid_json_value_end(end))
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

const char *static_radar_gate_operation_state_name(StaticRadarGateOperationState state) {
  switch (state) {
    case StaticRadarGateOperationState::PENDING:
      return "pending";
    case StaticRadarGateOperationState::DISPATCHING:
      return "pending";
    case StaticRadarGateOperationState::READBACK_SENT:
      return "verifying";
    case StaticRadarGateOperationState::APPLIED:
      return "applied";
    case StaticRadarGateOperationState::FAILED:
      return "failed";
    case StaticRadarGateOperationState::IDLE:
    default:
      return "idle";
  }
}

const char *static_radar_tuning_phase_name(StaticRadarTuningPhase phase) {
  switch (phase) {
    case StaticRadarTuningPhase::STARTING:
      return "starting";
    case StaticRadarTuningPhase::ACTIVE:
      return "active";
    case StaticRadarTuningPhase::STOPPING:
      return "stopping";
    case StaticRadarTuningPhase::FAILED:
      return "failed";
    case StaticRadarTuningPhase::IDLE:
    default:
      return "idle";
  }
}

}  // namespace

bool ControlHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET)
    return url == "/api/control/status" || url == "/api/control/static-radar-tuning";
  if (request->method() == HTTP_POST)
    return url == "/api/control/status-led" || url == "/api/control/led-duration" ||
           url == "/api/control/environment-correction" || url == "/api/control/temperature-offset" ||
           url == "/api/control/humidity-offset" || url == "/api/control/timezone" ||
           url == "/api/control/static-radar-tuning/session" ||
           url == "/api/control/static-radar-tuning/gate" ||
           url == "/api/control/static-radar-tuning/factory-reset";
  return false;
}

bool ControlHandler::handle(AsyncWebServerRequest *request, StringRef url) {
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

  if (request->method() == HTTP_POST && url == "/api/control/static-radar-tuning/factory-reset") {
    this->handle_static_radar_factory_reset_(request);
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
  stream->printf(R"({"ok":true,"available":%s,"recovering":%s,"active":%s,"phase":"%s","result":"%s","resolutionMm":%u,"gates":[)",
                 this->control_state_->static_radar_available ? "true" : "false",
                 this->control_state_->static_radar_recovering ? "true" : "false",
                 this->control_state_->static_radar_tuning_active ? "true" : "false",
                 static_radar_tuning_phase_name(this->control_state_->static_radar_tuning_phase),
                 this->control_state_->static_radar_tuning_result,
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

  stream->printf(
      R"(],"operation":{"id":%u,"state":"%s","result":"%s","gate":%u,"sensitivity":%u,"moveThreshold":)",
      static_cast<unsigned>(this->control_state_->static_radar_gate_operation_id),
      static_radar_gate_operation_state_name(this->control_state_->static_radar_gate_operation_state),
      this->control_state_->static_radar_gate_operation_result,
      static_cast<unsigned>(this->control_state_->requested_static_radar_gate),
      static_cast<unsigned>(this->control_state_->requested_static_radar_sensitivity));
  if (std::isfinite(this->control_state_->static_radar_gate_actual_move_threshold))
    stream->printf("%.0f", this->control_state_->static_radar_gate_actual_move_threshold);
  else
    stream->print("null");
  stream->print(R"(,"stillThreshold":)");
  if (std::isfinite(this->control_state_->static_radar_gate_actual_still_threshold))
    stream->printf("%.0f", this->control_state_->static_radar_gate_actual_still_threshold);
  else
    stream->print("null");
  stream->print("}}");
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
  if (this->control_state_->static_radar_recovering) {
    http_response::send_error_info(request, 409, "static_radar_recovering",
                                   "static_radar_recovering", "warning",
                                   R"({"target":"ld2410c"})");
    return;
  }

  const bool changed = this->control_state_->static_radar_tuning_requested_active != active;
  this->control_state_->static_radar_tuning_requested_active = active;
  this->control_state_->static_radar_tuning_keepalive_ms = active ? millis() : 0;
  this->control_state_->pending_static_radar_tuning_session =
      this->control_state_->pending_static_radar_tuning_session || changed;
  if (changed) {
    this->control_state_->static_radar_tuning_phase =
        active ? StaticRadarTuningPhase::STARTING : StaticRadarTuningPhase::STOPPING;
    this->control_state_->static_radar_tuning_result = "pending";
  }
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
  if (this->control_state_->static_radar_recovering) {
    http_response::send_error_info(request, 409, "static_radar_recovering",
                                   "static_radar_recovering", "warning",
                                   R"({"target":"ld2410c"})");
    return;
  }
  if (!this->control_state_->static_radar_tuning_active) {
    http_response::send_error_info(request, 409, "static_radar_tuning_inactive", "static_radar_tuning_inactive",
                                   "warning", R"({"target":"ld2410c"})");
    return;
  }

  uint32_t operation_id = 0;
  if (this->control_state_->request_static_radar_gate(
          static_cast<uint8_t>(gate), static_cast<uint8_t>(sensitivity), &operation_id) ==
      StaticRadarGateRequestResult::BUSY) {
    http_response::send_error_info(request, 409, "static_radar_tuning_busy", "static_radar_tuning_busy",
                                   "warning", R"({"target":"ld2410c"})");
    return;
  }

  char response[96];
  snprintf(response, sizeof(response), R"({"ok":true,"operationId":%u,"state":"pending"})",
           static_cast<unsigned>(operation_id));
  http_response::send_json(request, 200, response);
}

void ControlHandler::handle_static_radar_factory_reset_(AsyncWebServerRequest *request) {
  const auto result = this->control_state_->request_static_radar_factory_reset();
  if (result == StaticRadarFactoryResetRequestResult::UNAVAILABLE) {
    http_response::send_error_info(request, 409, "static_radar_unavailable", "static_radar_unavailable", "warning",
                                   R"({"target":"ld2410c"})");
    return;
  }
  if (result == StaticRadarFactoryResetRequestResult::ALREADY_PENDING) {
    http_response::send_error_info(request, 409, "static_radar_factory_reset_pending",
                                   "static_radar_factory_reset_pending", "warning",
                                   R"({"target":"ld2410c"})");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true,"restartExpected":true})");
}

}  // namespace radar_api_server
}  // namespace esphome
