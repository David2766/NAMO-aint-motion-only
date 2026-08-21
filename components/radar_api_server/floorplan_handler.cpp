#include "floorplan_handler.h"

#include "radar_api_server.h"
#include "http_response.h"
#include "strict_parse.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_http_server.h>
#include <memory>
#include <new>
#include <string>

namespace esphome {
namespace radar_api_server {

namespace {

static const char *const TAG = "radar_floorplan";
static constexpr uint32_t PAYLOAD_STREAM_CHUNK_SIZE = 512;
static constexpr size_t FLOORPLAN_RADAR_PATCH_MAX_SIZE = 512;
static constexpr size_t FLOORPLAN_OCCLUSION_PATCH_MAX_SIZE = 4096;
static constexpr size_t FLOORPLAN_OBJECTS_PATCH_MAX_SIZE = 16384;
static constexpr size_t FLOORPLAN_ROOM_ID_MAX_SIZE = 128;
static constexpr size_t FLOORPLAN_ROOM_NAME_MAX_SIZE = 128;

const char *upload_result_code(RadarUploadResult result) {
  switch (result) {
    case RadarUploadResult::OK:
      return "ok";
    case RadarUploadResult::INVALID_REQUEST:
      return "invalid_request";
    case RadarUploadResult::SESSION_MISMATCH:
      return "upload_session_mismatch";
    case RadarUploadResult::OFFSET_MISMATCH:
      return "upload_offset_mismatch";
    case RadarUploadResult::PAYLOAD_TOO_LARGE:
      return "upload_payload_too_large";
    case RadarUploadResult::INCOMPLETE:
      return "upload_incomplete";
    case RadarUploadResult::STORAGE_FAILED:
    case RadarUploadResult::READ_FAILED:
      return "upload_storage_failed";
  }
  return "internal_error";
}

int upload_result_http_code(RadarUploadResult result) {
  switch (result) {
    case RadarUploadResult::INVALID_REQUEST:
      return 400;
    case RadarUploadResult::SESSION_MISMATCH:
    case RadarUploadResult::OFFSET_MISMATCH:
    case RadarUploadResult::INCOMPLETE:
      return 409;
    case RadarUploadResult::PAYLOAD_TOO_LARGE:
      return 413;
    case RadarUploadResult::STORAGE_FAILED:
    case RadarUploadResult::READ_FAILED:
      return 500;
    case RadarUploadResult::OK:
      return 200;
  }
  return 500;
}

void send_upload_error(AsyncWebServerRequest *request, const char *legacy_error, RadarUploadResult result) {
  http_response::send_error_info(request, upload_result_http_code(result), legacy_error, upload_result_code(result),
                                 "error", "{}");
}

const char *payload_target_name(RadarPayloadTarget target) {
  switch (target) {
    case RadarPayloadTarget::CONFIG:
      return "config";
    case RadarPayloadTarget::IMAGE:
      return "image";
    case RadarPayloadTarget::DEVICE_CONFIG:
      return "device_config";
    case RadarPayloadTarget::STATS:
      return "stats";
  }
  return "unknown";
}

std::string target_detail_json(RadarPayloadTarget target) {
  std::string detail = R"({"target":")";
  detail += payload_target_name(target);
  detail += R"("})";
  return detail;
}

std::string field_target_detail_json(const char *field, const char *target) {
  std::string detail = R"({"field":")";
  detail += field;
  detail += R"(","target":")";
  detail += target;
  detail += R"("})";
  return detail;
}

bool is_json_ws(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

size_t skip_json_ws(const std::string &json, size_t pos) {
  while (pos < json.size() && is_json_ws(json[pos]))
    pos++;
  return pos;
}

bool find_json_string_end(const std::string &json, size_t start, size_t end, size_t *out) {
  if (start >= end || json[start] != '"')
    return false;
  bool escaped = false;
  for (size_t i = start + 1; i < end; i++) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (json[i] == '\\') {
      escaped = true;
      continue;
    }
    if (json[i] == '"') {
      *out = i + 1;
      return true;
    }
  }
  return false;
}

bool find_json_value_end(const std::string &json, size_t start, size_t end, size_t *out) {
  if (start >= end)
    return false;
  const char first = json[start];
  if (first == '"')
    return find_json_string_end(json, start, end, out);

  if (first == '{' || first == '[') {
    const char open = first;
    const char close = first == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = start; i < end; i++) {
      const char c = json[i];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == open) {
        depth++;
      } else if (c == close) {
        depth--;
        if (depth == 0) {
          *out = i + 1;
          return true;
        }
      }
    }
    return false;
  }

  size_t pos = start;
  while (pos < end && json[pos] != ',' && json[pos] != '}' && json[pos] != ']')
    pos++;
  *out = pos;
  return pos > start;
}

bool find_json_object_range(const std::string &json, size_t *start, size_t *end) {
  const size_t object_start = skip_json_ws(json, 0);
  if (object_start >= json.size() || json[object_start] != '{')
    return false;
  size_t object_end = 0;
  if (!find_json_value_end(json, object_start, json.size(), &object_end))
    return false;
  if (skip_json_ws(json, object_end) != json.size())
    return false;
  *start = object_start;
  *end = object_end;
  return true;
}

bool json_key_matches(const std::string &json, size_t key_start, size_t key_end, const char *key) {
  const size_t key_size = key_end - key_start;
  const size_t expected_size = std::strlen(key);
  return key_size == expected_size && json.compare(key_start, key_size, key) == 0;
}

bool find_json_member_value_range(const std::string &json, size_t object_start, size_t object_end, const char *key,
                                  size_t *value_start, size_t *value_end) {
  if (object_start >= object_end || json[object_start] != '{')
    return false;

  size_t pos = object_start + 1;
  while (pos < object_end) {
    pos = skip_json_ws(json, pos);
    if (pos >= object_end || json[pos] == '}')
      break;
    if (json[pos] != '"')
      return false;

    size_t key_string_end = 0;
    if (!find_json_string_end(json, pos, object_end, &key_string_end))
      return false;
    const size_t key_start = pos + 1;
    const size_t key_end = key_string_end - 1;
    pos = skip_json_ws(json, key_string_end);
    if (pos >= object_end || json[pos] != ':')
      return false;
    const size_t candidate_value_start = skip_json_ws(json, pos + 1);
    size_t candidate_value_end = 0;
    if (!find_json_value_end(json, candidate_value_start, object_end, &candidate_value_end))
      return false;
    if (json_key_matches(json, key_start, key_end, key)) {
      *value_start = candidate_value_start;
      *value_end = candidate_value_end;
      return true;
    }
    pos = skip_json_ws(json, candidate_value_end);
    if (pos < object_end && json[pos] == ',')
      pos++;
  }
  return false;
}

std::string json_string_literal(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
          out += escaped;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool json_string_value_equals(const std::string &json, size_t value_start, size_t value_end, const std::string &expected) {
  return value_start < value_end && json[value_start] == '"' &&
         json.compare(value_start, value_end - value_start, json_string_literal(expected)) == 0;
}

bool is_json_object_like(const std::string &value) {
  size_t start = 0;
  size_t end = 0;
  return find_json_object_range(value, &start, &end) && start == skip_json_ws(value, 0) && end <= value.size();
}

bool is_json_array_like(const std::string &value) {
  const size_t array_start = skip_json_ws(value, 0);
  if (array_start >= value.size() || value[array_start] != '[')
    return false;
  size_t array_end = 0;
  if (!find_json_value_end(value, array_start, value.size(), &array_end))
    return false;
  return skip_json_ws(value, array_end) == value.size();
}

const char *payload_not_found_code(RadarPayloadTarget target) {
  return target == RadarPayloadTarget::CONFIG ? "config_not_found" : "not_found";
}

}  // namespace

bool FloorplanHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET) {
    return url == "/api/floorplan/status" || url == "/api/floorplan" || url == "/api/floorplan/image";
  }

  if (request->method() == HTTP_POST) {
    return url == "/api/floorplan/upload/start" || url == "/api/floorplan/upload/chunk" ||
           url == "/api/floorplan/upload/commit" || url == "/api/floorplan/radar" ||
           url == "/api/floorplan/room-name" || url == "/api/floorplan/occlusion" ||
           url == "/api/floorplan/objects" || url == "/api/floorplan/delete";
  }

  return false;
}

bool FloorplanHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (request->method() == HTTP_GET) {
    if (url == "/api/floorplan/status") {
      this->handle_status_(request);
      return true;
    }
    if (url == "/api/floorplan") {
      this->handle_get_payload_(request, RadarPayloadTarget::CONFIG, "application/json", "config_not_found",
                                "config_read_failed");
      return true;
    }
    if (url == "/api/floorplan/image") {
      this->handle_get_payload_(request, RadarPayloadTarget::IMAGE, "image/webp", "image_not_found",
                                "image_read_failed");
      return true;
    }
  }

  if (request->method() == HTTP_POST) {
    if (url == "/api/floorplan/radar") {
      this->handle_patch_radar_(request);
      return true;
    }
    if (!this->allow_shared_write_(request))
      return true;
    if (url == "/api/floorplan/upload/start") {
      this->handle_upload_start_(request);
      return true;
    }
    if (url == "/api/floorplan/upload/chunk") {
      this->handle_upload_chunk_(request);
      return true;
    }
    if (url == "/api/floorplan/upload/commit") {
      this->handle_upload_commit_(request);
      return true;
    }
    if (url == "/api/floorplan/room-name") {
      this->handle_patch_room_name_(request);
      return true;
    }
    if (url == "/api/floorplan/occlusion") {
      this->handle_patch_occlusion_(request);
      return true;
    }
    if (url == "/api/floorplan/objects") {
      this->handle_patch_objects_(request);
      return true;
    }
    if (url == "/api/floorplan/delete") {
      this->handle_delete_storage_(request);
      return true;
    }
  }

  return false;
}

bool FloorplanHandler::allow_shared_write_(
    AsyncWebServerRequest *request) const {
  if (this->server_ == nullptr || !this->server_->site_configuration_ready())
    return true;
  const auto &configuration = this->server_->site_configuration_snapshot();
  if (configuration.configuration_owner_node_id[0] != '\0' &&
      std::strcmp(configuration.configuration_owner_node_id,
                  this->server_->platform_identity().node_id) != 0) {
    http_response::send_error_info(
        request, 403, "site_configuration_owner_required",
        "site_configuration_owner_required", "warning", "{}");
    return false;
  }
  if (!this->server_->shared_site_configuration_writable(millis())) {
    http_response::send_error_info(
        request, 409, "site_configuration_continuity_unavailable",
        "site_configuration_continuity_unavailable", "warning", "{}");
    return false;
  }
  return true;
}

void FloorplanHandler::handle_status_(AsyncWebServerRequest *request) {
  const auto status = this->storage_->status();
  const auto &header = status.header;

  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      R"({"ok":%s,"storage":"raw-partition","partition":"%s","hasConfig":%s,"hasImage":%s,"totalBytes":%u,"usedBytes":%u,"configBytes":%u,"imageBytes":%u,"configHash":%u,"imageHash":%u,"uploadTarget":%u,"uploadSize":%u,"uploadWritten":%u})",
      status.ok ? "true" : "false", status.partition_label, header.config_size > 0 ? "true" : "false",
      header.image_size > 0 ? "true" : "false", static_cast<unsigned>(status.total_bytes),
      static_cast<unsigned>(status.used_bytes), static_cast<unsigned>(header.config_size),
      static_cast<unsigned>(header.image_size), static_cast<unsigned>(header.config_hash),
      static_cast<unsigned>(header.image_hash), static_cast<unsigned>(header.upload_target),
      static_cast<unsigned>(header.upload_size), static_cast<unsigned>(header.upload_written));
  request->send(stream);
}

void FloorplanHandler::handle_get_payload_(AsyncWebServerRequest *request, RadarPayloadTarget target,
                                                const char *content_type, const char *not_found_error,
                                                const char *read_error) {
  uint32_t payload_offset = 0;
  uint32_t payload_size = 0;
  if (!this->storage_->payload_info(target, &payload_offset, &payload_size)) {
    const std::string detail = target_detail_json(target);
    http_response::send_error_info(request, 404, not_found_error, payload_not_found_code(target), "error",
                                   detail.c_str());
    return;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[PAYLOAD_STREAM_CHUNK_SIZE]);
  if (!buffer) {
    ESP_LOGE(TAG, "floorplan payload stream buffer allocation failed target=%u size=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(PAYLOAD_STREAM_CHUNK_SIZE));
    const std::string detail = target_detail_json(target);
    http_response::send_error_info(request, 500, read_error, "internal_error", "error", detail.c_str());
    return;
  }

  httpd_req_t *raw_request = *request;
  httpd_resp_set_type(raw_request, content_type);
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");

  uint32_t sent = 0;
  while (sent < payload_size) {
    const uint32_t chunk_size = std::min<uint32_t>(PAYLOAD_STREAM_CHUNK_SIZE, payload_size - sent);
    if (!this->storage_->read_storage_range(payload_offset + sent, buffer.get(), chunk_size)) {
      ESP_LOGE(TAG, "floorplan payload read failed target=%u offset=%u size=%u", static_cast<unsigned>(target),
               static_cast<unsigned>(payload_offset + sent), static_cast<unsigned>(chunk_size));
      httpd_resp_send_chunk(raw_request, nullptr, 0);
      return;
    }
    if (httpd_resp_send_chunk(raw_request, reinterpret_cast<const char *>(buffer.get()), chunk_size) != ESP_OK) {
      ESP_LOGE(TAG, "floorplan payload send failed target=%u sent=%u size=%u", static_cast<unsigned>(target),
               static_cast<unsigned>(sent), static_cast<unsigned>(payload_size));
      return;
    }
    sent += chunk_size;
  }

  if (httpd_resp_send_chunk(raw_request, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "floorplan payload final chunk failed target=%u size=%u", static_cast<unsigned>(target),
             static_cast<unsigned>(payload_size));
    return;
  }
}

void FloorplanHandler::handle_upload_start_(AsyncWebServerRequest *request) {
  RadarPayloadTarget target{};
  if (!this->parse_floorplan_target_(request, &target)) {
    http_response::send_error_info(request, 400, "invalid_target", "invalid_request", "error", "{}");
    return;
  }
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error", "{}");
    return;
  }
  if (!request->hasArg("size")) {
    http_response::send_error_info(request, 400, "missing_size", "invalid_request", "error", "{}");
    return;
  }

  uint32_t size = 0;
  if (!parse_uint32_strict(request->arg("size"), &size, false, false)) {
    http_response::send_error_info(request, 400, "invalid_size",
                                   "invalid_request", "error", "{}");
    return;
  }
  if (size > this->storage_->payload_max_size(target)) {
    http_response::send_error_info(request, 413, "payload_too_large", "upload_payload_too_large", "error", "{}");
    return;
  }

  if (!this->storage_->start_upload(target, size, session_id)) {
    send_upload_error(request, "erase_failed", this->storage_->last_upload_result());
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_upload_chunk_(AsyncWebServerRequest *request) {
  RadarPayloadTarget target{};
  if (!this->parse_floorplan_target_(request, &target)) {
    http_response::send_error_info(request, 400, "invalid_target", "invalid_request", "error", "{}");
    return;
  }
  if (!request->hasArg("offset") || !request->hasArg("data")) {
    http_response::send_error_info(request, 400, "missing_chunk", "invalid_request", "error", "{}");
    return;
  }
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error", "{}");
    return;
  }

  uint32_t offset = 0;
  if (!parse_uint32_strict(request->arg("offset"), &offset)) {
    http_response::send_error_info(request, 400, "invalid_offset",
                                   "invalid_request", "error", "{}");
    return;
  }
  std::string decoded;
  if (!this->decode_hex_(request->arg("data"), &decoded)) {
    http_response::send_error_info(request, 400, "invalid_hex", "invalid_request", "error", "{}");
    return;
  }

  if (!this->storage_->write_upload_chunk(target, offset, reinterpret_cast<const uint8_t *>(decoded.data()),
                                          decoded.size(), session_id)) {
    send_upload_error(request, "chunk_write_failed", this->storage_->last_upload_result());
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_upload_commit_(AsyncWebServerRequest *request) {
  RadarPayloadTarget target{};
  if (!this->parse_floorplan_target_(request, &target)) {
    http_response::send_error_info(request, 400, "invalid_target", "invalid_request", "error", "{}");
    return;
  }
  uint32_t session_id = 0;
  if (!this->parse_upload_session_(request, &session_id)) {
    http_response::send_error_info(request, 400, "invalid_session", "invalid_request", "error", "{}");
    return;
  }

  if (!this->storage_->commit_upload(target, session_id)) {
    send_upload_error(request, "upload_incomplete", this->storage_->last_upload_result());
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_patch_radar_(AsyncWebServerRequest *request) {
  if (!request->hasArg("data")) {
    const std::string detail = field_target_detail_json("data", "floorplan_radar");
    http_response::send_error_info(request, 400, "missing_data", "invalid_request", "error", detail.c_str());
    return;
  }

  const std::string radar = request->arg("data");
  if (radar.empty() || radar.size() > FLOORPLAN_RADAR_PATCH_MAX_SIZE || !is_json_object_like(radar) ||
      radar.find("\"originPx\"") == std::string::npos || radar.find("\"rotationDeg\"") == std::string::npos ||
      radar.find("\"scale\"") == std::string::npos) {
    const std::string detail = field_target_detail_json("data", "floorplan_radar");
    http_response::send_error_info(request, 400, "invalid_data", "invalid_request", "error", detail.c_str());
    return;
  }

  std::string document;
  if (!this->patch_floorplan_member_("radar", radar, &document)) {
    http_response::send_error_info(request, 500, "patch_failed", "internal_error", "error",
                                   R"({"target":"floorplan","field":"radar"})");
    return;
  }
  if (!this->write_floorplan_document_(document)) {
    http_response::send_error_info(request, 500, "save_failed", "upload_storage_failed", "error",
                                   R"({"target":"floorplan","where":"radar_patch"})");
    return;
  }
  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_patch_room_name_(AsyncWebServerRequest *request) {
  if (!request->hasArg("id") || !request->hasArg("name")) {
    const std::string detail = field_target_detail_json(!request->hasArg("id") ? "id" : "name", "floorplan_room_name");
    http_response::send_error_info(request, 400, "missing_field", "invalid_request", "error", detail.c_str());
    return;
  }

  const std::string room_id = request->arg("id");
  const std::string name = request->arg("name");
  if (room_id.empty() || room_id.size() > FLOORPLAN_ROOM_ID_MAX_SIZE || name.size() > FLOORPLAN_ROOM_NAME_MAX_SIZE) {
    const std::string detail = field_target_detail_json(room_id.empty() ? "id" : "name", "floorplan_room_name");
    http_response::send_error_info(request, 400, "invalid_field", "invalid_request", "error", detail.c_str());
    return;
  }

  std::string document;
  if (!this->patch_floorplan_room_name_(room_id, name, &document)) {
    http_response::send_error_info(request, 404, "room_not_found", "not_found", "error",
                                   R"({"target":"floorplan","field":"room"})");
    return;
  }
  if (!this->write_floorplan_document_(document)) {
    http_response::send_error_info(request, 500, "save_failed", "upload_storage_failed", "error",
                                   R"({"target":"floorplan","where":"room_name_patch"})");
    return;
  }
  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_patch_occlusion_(AsyncWebServerRequest *request) {
  if (!request->hasArg("data")) {
    const std::string detail = field_target_detail_json("data", "floorplan_occlusion");
    http_response::send_error_info(request, 400, "missing_data", "invalid_request", "error", detail.c_str());
    return;
  }

  const std::string occlusion = request->arg("data");
  if (occlusion.empty() || occlusion.size() > FLOORPLAN_OCCLUSION_PATCH_MAX_SIZE || !is_json_object_like(occlusion) ||
      occlusion.find("\"ignoredEdges\"") == std::string::npos) {
    const std::string detail = field_target_detail_json("data", "floorplan_occlusion");
    http_response::send_error_info(request, 400, "invalid_data", "invalid_request", "error", detail.c_str());
    return;
  }

  std::string document;
  if (!this->patch_floorplan_member_("occlusion", occlusion, &document)) {
    http_response::send_error_info(request, 500, "patch_failed", "internal_error", "error",
                                   R"({"target":"floorplan","field":"occlusion"})");
    return;
  }
  if (!this->write_floorplan_document_(document)) {
    http_response::send_error_info(request, 500, "save_failed", "upload_storage_failed", "error",
                                   R"({"target":"floorplan","where":"occlusion_patch"})");
    return;
  }
  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_patch_objects_(AsyncWebServerRequest *request) {
  if (!request->hasArg("data")) {
    const std::string detail = field_target_detail_json("data", "floorplan_objects");
    http_response::send_error_info(request, 400, "missing_data", "invalid_request", "error", detail.c_str());
    return;
  }

  const std::string objects = request->arg("data");
  if (objects.size() > FLOORPLAN_OBJECTS_PATCH_MAX_SIZE || !is_json_array_like(objects)) {
    const std::string detail = field_target_detail_json("data", "floorplan_objects");
    http_response::send_error_info(request, 400, "invalid_data", "invalid_request", "error", detail.c_str());
    return;
  }

  std::string document;
  if (!this->patch_floorplan_member_("objects", objects, &document)) {
    http_response::send_error_info(request, 500, "patch_failed", "internal_error", "error",
                                   R"({"target":"floorplan","field":"objects"})");
    return;
  }
  if (!this->write_floorplan_document_(document)) {
    http_response::send_error_info(request, 500, "save_failed", "upload_storage_failed", "error",
                                   R"({"target":"floorplan","where":"objects_patch"})");
    return;
  }
  http_response::send_json(request, 200, R"({"ok":true})");
}

void FloorplanHandler::handle_delete_storage_(AsyncWebServerRequest *request) {
  if (!request->hasArg("confirm") || request->arg("confirm") != "1") {
    const std::string detail = field_target_detail_json("confirm", "floorplan_delete");
    http_response::send_error_info(request, 400, "missing_confirm", "invalid_request", "error", detail.c_str());
    return;
  }
  if (this->server_ != nullptr &&
      this->server_->site_configuration_ready() &&
      site_configuration_has_remote_members(
          this->server_->platform_identity(),
          this->server_->site_configuration_snapshot())) {
    http_response::send_error_info(
        request, 409, "floorplan_delete_requires_standalone",
        "floorplan_delete_requires_standalone", "warning", "{}");
    return;
  }
  if (this->server_ != nullptr &&
      this->server_->site_configuration_ready() &&
      std::strcmp(
          this->server_->site_configuration_snapshot()
              .configuration_owner_node_id,
          this->server_->platform_identity().node_id) == 0 &&
      !this->server_->shared_site_configuration_writable(millis())) {
    http_response::send_error_info(
        request, 409, "site_configuration_continuity_unavailable",
        "site_configuration_continuity_unavailable", "warning", "{}");
    return;
  }

  if (!this->storage_->delete_floorplan()) {
    http_response::send_error_info(request, 500, "delete_failed", "internal_error", "error",
                                   R"({"target":"floorplan","where":"delete_floorplan"})");
    return;
  }
  if (this->server_ != nullptr &&
      !this->server_->clear_owned_floorplan_manifest()) {
    http_response::send_error_info(
        request, 500, "delete_manifest_failed", "internal_error", "error",
        R"({"target":"floorplan","where":"clear_shared_manifest"})");
    return;
  }

  http_response::send_json(request, 200, R"({"ok":true})");
}

bool FloorplanHandler::parse_floorplan_target_(AsyncWebServerRequest *request, RadarPayloadTarget *target) const {
  if (!request->hasArg("target") || !this->storage_->parse_target(request->arg("target"), target))
    return false;
  return *target == RadarPayloadTarget::CONFIG || *target == RadarPayloadTarget::IMAGE;
}

bool FloorplanHandler::parse_upload_session_(AsyncWebServerRequest *request, uint32_t *session_id) const {
  if (!request->hasArg("session"))
    return false;
  uint32_t parsed = 0;
  if (!parse_uint32_strict(request->arg("session"), &parsed, true, false))
    return false;
  *session_id = parsed;
  return true;
}

bool FloorplanHandler::patch_floorplan_member_(const char *key, const std::string &value, std::string *out) const {
  std::string document;
  if (!this->storage_->read_payload(RadarPayloadTarget::CONFIG, &document))
    return false;

  size_t object_start = 0;
  size_t object_end = 0;
  if (!find_json_object_range(document, &object_start, &object_end))
    return false;

  size_t value_start = 0;
  size_t value_end = 0;
  if (!find_json_member_value_range(document, object_start, object_end, key, &value_start, &value_end))
    return false;

  document.replace(value_start, value_end - value_start, value);
  *out = std::move(document);
  return true;
}

bool FloorplanHandler::patch_floorplan_room_name_(const std::string &room_id, const std::string &name,
                                                  std::string *out) const {
  std::string document;
  if (!this->storage_->read_payload(RadarPayloadTarget::CONFIG, &document))
    return false;

  size_t object_start = 0;
  size_t object_end = 0;
  if (!find_json_object_range(document, &object_start, &object_end))
    return false;

  size_t rooms_start = 0;
  size_t rooms_end = 0;
  if (!find_json_member_value_range(document, object_start, object_end, "rooms", &rooms_start, &rooms_end))
    return false;
  if (rooms_start >= rooms_end || document[rooms_start] != '[')
    return false;

  size_t pos = rooms_start + 1;
  while (pos < rooms_end) {
    pos = skip_json_ws(document, pos);
    if (pos >= rooms_end || document[pos] == ']')
      break;
    if (document[pos] != '{')
      return false;
    size_t room_end = 0;
    if (!find_json_value_end(document, pos, rooms_end, &room_end))
      return false;

    size_t id_start = 0;
    size_t id_end = 0;
    if (find_json_member_value_range(document, pos, room_end, "id", &id_start, &id_end) &&
        json_string_value_equals(document, id_start, id_end, room_id)) {
      size_t name_start = 0;
      size_t name_end = 0;
      if (!find_json_member_value_range(document, pos, room_end, "name", &name_start, &name_end))
        return false;
      document.replace(name_start, name_end - name_start, json_string_literal(name));
      *out = std::move(document);
      return true;
    }

    pos = skip_json_ws(document, room_end);
    if (pos < rooms_end && document[pos] == ',')
      pos++;
  }
  return false;
}

bool FloorplanHandler::write_floorplan_document_(const std::string &document) const {
  if (document.empty())
    return false;
  return this->storage_->write_payload(RadarPayloadTarget::CONFIG,
                                       reinterpret_cast<const uint8_t *>(document.data()), document.size());
}

bool FloorplanHandler::decode_hex_(const std::string &hex, std::string *out) const {
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
