#include "shared_automation_area.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

struct JsonSpan {
  const char *data{nullptr};
  std::size_t size{0};
};

bool is_ws(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::size_t skip_ws(JsonSpan span, std::size_t offset) {
  while (offset < span.size && is_ws(span.data[offset]))
    ++offset;
  return offset;
}

bool string_end(JsonSpan span, std::size_t offset, std::size_t *end) {
  if (end == nullptr || offset >= span.size || span.data[offset] != '"')
    return false;
  bool escaped = false;
  for (std::size_t index = offset + 1; index < span.size; ++index) {
    const char value = span.data[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (value == '\\') {
      escaped = true;
      continue;
    }
    if (value == '"') {
      *end = index + 1;
      return true;
    }
  }
  return false;
}

bool value_end(JsonSpan span, std::size_t offset, std::size_t *end) {
  if (end == nullptr)
    return false;
  offset = skip_ws(span, offset);
  if (offset >= span.size)
    return false;
  if (span.data[offset] == '"')
    return string_end(span, offset, end);
  if (span.data[offset] == '{' || span.data[offset] == '[') {
    const char open = span.data[offset];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = offset; index < span.size; ++index) {
      const char value = span.data[index];
      if (in_string) {
        if (escaped)
          escaped = false;
        else if (value == '\\')
          escaped = true;
        else if (value == '"')
          in_string = false;
        continue;
      }
      if (value == '"')
        in_string = true;
      else if (value == open)
        ++depth;
      else if (value == close && --depth == 0) {
        *end = index + 1;
        return true;
      }
    }
    return false;
  }
  std::size_t index = offset;
  while (index < span.size && span.data[index] != ',' &&
         span.data[index] != '}' && span.data[index] != ']')
    ++index;
  while (index > offset && is_ws(span.data[index - 1]))
    --index;
  *end = index;
  return index > offset;
}

bool object_span(JsonSpan span, JsonSpan *object) {
  if (object == nullptr)
    return false;
  const std::size_t start = skip_ws(span, 0);
  std::size_t end = 0;
  if (start >= span.size || span.data[start] != '{' ||
      !value_end(span, start, &end) || skip_ws(span, end) != span.size)
    return false;
  *object = {span.data + start, end - start};
  return true;
}

bool key_equals(JsonSpan span, std::size_t start, std::size_t end,
                const char *key) {
  const std::size_t length = std::strlen(key);
  return end >= start && end - start == length &&
         std::memcmp(span.data + start, key, length) == 0;
}

bool member(JsonSpan object, const char *key, JsonSpan *value) {
  if (value == nullptr || object.size < 2 || object.data[0] != '{')
    return false;
  std::size_t offset = 1;
  while (offset < object.size) {
    offset = skip_ws(object, offset);
    if (offset >= object.size || object.data[offset] == '}')
      return false;
    std::size_t key_end = 0;
    if (object.data[offset] != '"' || !string_end(object, offset, &key_end))
      return false;
    const std::size_t key_start = offset + 1;
    const std::size_t key_content_end = key_end - 1;
    offset = skip_ws(object, key_end);
    if (offset >= object.size || object.data[offset] != ':')
      return false;
    const std::size_t value_start = skip_ws(object, offset + 1);
    std::size_t candidate_end = 0;
    if (!value_end(object, value_start, &candidate_end))
      return false;
    if (key_equals(object, key_start, key_content_end, key)) {
      *value = {object.data + value_start, candidate_end - value_start};
      return true;
    }
    offset = skip_ws(object, candidate_end);
    if (offset < object.size && object.data[offset] == ',')
      ++offset;
  }
  return false;
}

bool copy_string(JsonSpan value, char *output, std::size_t capacity) {
  if (output == nullptr || capacity == 0 || value.size < 2 ||
      value.data[0] != '"' || value.data[value.size - 1] != '"')
    return false;
  std::size_t written = 0;
  for (std::size_t index = 1; index + 1 < value.size; ++index) {
    char current = value.data[index];
    if (current == '\\') {
      if (++index + 1 >= value.size)
        return false;
      switch (value.data[index]) {
        case '"': current = '"'; break;
        case '\\': current = '\\'; break;
        case '/': current = '/'; break;
        case 'b': current = '\b'; break;
        case 'f': current = '\f'; break;
        case 'n': current = '\n'; break;
        case 'r': current = '\r'; break;
        case 't': current = '\t'; break;
        default: return false;
      }
    }
    if (written + 1 >= capacity)
      return false;
    output[written++] = current;
  }
  output[written] = '\0';
  return true;
}

bool valid_identifier(const char *value) {
  if (value == nullptr || value[0] == '\0')
    return false;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    const char current = *cursor;
    if (!((current >= 'a' && current <= 'z') ||
          (current >= 'A' && current <= 'Z') ||
          (current >= '0' && current <= '9') || current == '.' ||
          current == '_' || current == ':' || current == '-'))
      return false;
  }
  return true;
}

bool uint32_value(JsonSpan value, uint32_t *output) {
  if (output == nullptr || value.size == 0 || value.size >= 16)
    return false;
  char buffer[16]{};
  std::memcpy(buffer, value.data, value.size);
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(buffer, &end, 10);
  if (end == buffer || *end != '\0' || parsed == 0 || parsed > UINT32_MAX)
    return false;
  *output = static_cast<uint32_t>(parsed);
  return true;
}

bool bool_value(JsonSpan value, bool *output) {
  if (output == nullptr)
    return false;
  if (value.size == 4 && std::memcmp(value.data, "true", 4) == 0) {
    *output = true;
    return true;
  }
  if (value.size == 5 && std::memcmp(value.data, "false", 5) == 0) {
    *output = false;
    return true;
  }
  return false;
}

bool float_value(JsonSpan value, float *output) {
  if (output == nullptr || value.size == 0 || value.size >= 32)
    return false;
  char buffer[32]{};
  std::memcpy(buffer, value.data, value.size);
  char *end = nullptr;
  const float parsed = std::strtof(buffer, &end);
  if (end == buffer || *end != '\0' || !std::isfinite(parsed) ||
      std::fabs(parsed) > 100000.0f)
    return false;
  *output = parsed;
  return true;
}

bool next_array_value(JsonSpan array, std::size_t *offset, JsonSpan *value) {
  if (offset == nullptr || value == nullptr || array.size < 2 ||
      array.data[0] != '[')
    return false;
  std::size_t cursor = skip_ws(array, *offset);
  if (*offset != 1) {
    if (cursor >= array.size || array.data[cursor] == ']') {
      *offset = cursor;
      return false;
    }
    if (array.data[cursor] != ',')
      return false;
    cursor = skip_ws(array, cursor + 1);
  }
  if (cursor >= array.size || array.data[cursor] == ']') {
    *offset = cursor;
    return false;
  }
  std::size_t end = 0;
  if (!value_end(array, cursor, &end))
    return false;
  *value = {array.data + cursor, end - cursor};
  *offset = end;
  return true;
}

bool array_finished(JsonSpan array, std::size_t offset) {
  offset = skip_ws(array, offset);
  return offset < array.size && array.data[offset] == ']' &&
         skip_ws(array, offset + 1) == array.size;
}

bool parse_points(JsonSpan value, const FloorplanRadarPose &pose,
                  SharedAutomationArea *area) {
  if (area == nullptr || value.size < 2 || value.data[0] != '[' ||
      !pose.valid || !std::isfinite(pose.scale_factor) ||
      pose.scale_factor <= 0.0f)
    return false;
  const float radians = pose.rotation_deg * 3.14159265358979323846f / 180.0f;
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  std::size_t offset = 1;
  JsonSpan point;
  while (next_array_value(value, &offset, &point)) {
    if (area->point_count >= area->x_mm.size() || point.size < 2 ||
        point.data[0] != '[')
      return false;
    std::size_t point_offset = 1;
    JsonSpan x_value;
    JsonSpan y_value;
    JsonSpan extra;
    float x_px = 0.0f;
    float y_px = 0.0f;
    if (!next_array_value(point, &point_offset, &x_value) ||
        !next_array_value(point, &point_offset, &y_value) ||
        next_array_value(point, &point_offset, &extra) ||
        !array_finished(point, point_offset) ||
        !float_value(x_value, &x_px) || !float_value(y_value, &y_px))
      return false;
    const float world_x =
        (x_px - pose.origin_x_px) * pose.mm_per_px_x / pose.scale_factor;
    const float world_y =
        (y_px - pose.origin_y_px) * pose.mm_per_px_y / pose.scale_factor;
    area->x_mm[area->point_count] = world_x * cosine + world_y * sine;
    area->y_mm[area->point_count] = world_x * sine - world_y * cosine;
    ++area->point_count;
  }
  return area->point_count >= 3 && array_finished(value, offset);
}

uint32_t hash_identifier(const char *value) {
  uint32_t hash = 2166136261U;
  for (const uint8_t *cursor = reinterpret_cast<const uint8_t *>(value);
       *cursor != 0; ++cursor) {
    hash ^= *cursor;
    hash *= 16777619U;
  }
  return hash;
}

}  // namespace

bool SharedAutomationArea::contains(float x, float y) const {
  if (!this->occupied || this->point_count < 3 || !std::isfinite(x) ||
      !std::isfinite(y))
    return false;
  bool inside = false;
  for (int index = 0, previous = this->point_count - 1;
       index < this->point_count; previous = index++) {
    const float denominator = this->y_mm[previous] - this->y_mm[index];
    const bool intersects =
        ((this->y_mm[index] > y) != (this->y_mm[previous] > y)) &&
        (x < (this->x_mm[previous] - this->x_mm[index]) *
                     (y - this->y_mm[index]) /
                     (std::fabs(denominator) < 0.0001f ? 0.0001f
                                                       : denominator) +
                 this->x_mm[index]);
    if (intersects)
      inside = !inside;
  }
  return inside;
}

bool SharedAutomationAreaCache::update(
    const char *json, std::size_t size, const char *expected_floorplan_id,
    const char *space_id, uint32_t config_hash,
    const FloorplanRadarPose &pose) {
  this->clear();
  this->attempted_ = true;
  this->config_hash_ = config_hash;
  this->pose_version_ = pose.pose_version;
  std::strncpy(this->floorplan_id_,
               expected_floorplan_id != nullptr ? expected_floorplan_id : "",
               sizeof(this->floorplan_id_) - 1);
  std::strncpy(this->space_id_, space_id != nullptr ? space_id : "",
               sizeof(this->space_id_) - 1);
  if (json == nullptr || size == 0 || !valid_identifier(this->floorplan_id_) ||
      !valid_identifier(this->space_id_) || config_hash == 0)
    return false;

  JsonSpan root;
  if (!object_span({json, size}, &root))
    return false;
  JsonSpan floorplan_value;
  char document_floorplan_id[SharedAutomationArea::MAX_ID_LENGTH + 1]{};
  if (!member(root, "floorplanId", &floorplan_value) ||
      !copy_string(floorplan_value, document_floorplan_id,
                   sizeof(document_floorplan_id)) ||
      std::strcmp(document_floorplan_id, this->floorplan_id_) != 0)
    return false;

  JsonSpan collection_value;
  if (!member(root, "automationAreas", &collection_value)) {
    this->configuration_valid_ = true;
    return true;
  }
  JsonSpan collection;
  if (!object_span(collection_value, &collection))
    return false;
  JsonSpan schema_value;
  JsonSpan revision_value;
  JsonSpan areas_value;
  uint32_t schema_version = 0;
  if (!member(collection, "schemaVersion", &schema_value) ||
      !uint32_value(schema_value, &schema_version) || schema_version != 1 ||
      !member(collection, "revision", &revision_value) ||
      !uint32_value(revision_value, &this->revision_) ||
      !member(collection, "areas", &areas_value) || areas_value.size < 2 ||
      areas_value.data[0] != '[')
    return false;

  std::array<uint32_t, 48> area_hashes{};
  uint8_t total_area_count = 0;
  std::size_t offset = 1;
  JsonSpan area_value;
  while (next_array_value(areas_value, &offset, &area_value)) {
    if (total_area_count >= area_hashes.size())
      return false;
    JsonSpan object;
    JsonSpan id_value;
    JsonSpan name_value;
    JsonSpan space_value;
    JsonSpan enabled_value;
    JsonSpan points_value;
    char area_id[SharedAutomationArea::MAX_ID_LENGTH + 1]{};
    char name[SharedAutomationArea::MAX_NAME_LENGTH + 1]{};
    char candidate_space_id[SharedAutomationArea::MAX_ID_LENGTH + 1]{};
    bool enabled = false;
    if (!object_span(area_value, &object) ||
        !member(object, "areaId", &id_value) ||
        !copy_string(id_value, area_id, sizeof(area_id)) ||
        !valid_identifier(area_id) || !member(object, "name", &name_value) ||
        !copy_string(name_value, name, sizeof(name)) ||
        !member(object, "spaceId", &space_value) ||
        !copy_string(space_value, candidate_space_id,
                     sizeof(candidate_space_id)) ||
        !valid_identifier(candidate_space_id) ||
        !member(object, "enabled", &enabled_value) ||
        !bool_value(enabled_value, &enabled) ||
        !member(object, "pointsPx", &points_value))
      return false;
    const uint32_t identifier_hash = hash_identifier(area_id);
    for (uint8_t index = 0; index < total_area_count; ++index) {
      if (area_hashes[index] == identifier_hash)
        return false;
    }
    area_hashes[total_area_count++] = identifier_hash;
    if (!enabled || std::strcmp(candidate_space_id, this->space_id_) != 0)
      continue;
    if (this->area_count_ >= this->areas_.size())
      return false;
    SharedAutomationArea parsed;
    std::strncpy(parsed.area_id, area_id, sizeof(parsed.area_id) - 1);
    std::strncpy(parsed.name, name, sizeof(parsed.name) - 1);
    if (!parse_points(points_value, pose, &parsed))
      return false;
    parsed.occupied = true;
    this->areas_[this->area_count_++] = parsed;
  }
  if (!array_finished(areas_value, offset))
    return false;
  this->configuration_valid_ = true;
  return true;
}

void SharedAutomationAreaCache::clear() {
  this->attempted_ = false;
  this->configuration_valid_ = false;
  this->floorplan_id_[0] = '\0';
  this->space_id_[0] = '\0';
  this->revision_ = 0;
  this->config_hash_ = 0;
  this->pose_version_ = 0;
  this->area_count_ = 0;
  this->areas_ = {};
}

bool SharedAutomationAreaCache::matches(const char *floorplan_id,
                                        const char *space_id,
                                        uint32_t config_hash,
                                        uint32_t pose_version) const {
  return this->attempted_ && this->config_hash_ == config_hash &&
         this->pose_version_ == pose_version &&
         std::strcmp(this->floorplan_id_,
                     floorplan_id != nullptr ? floorplan_id : "") == 0 &&
         std::strcmp(this->space_id_, space_id != nullptr ? space_id : "") ==
             0;
}

const SharedAutomationArea &SharedAutomationAreaCache::area(
    std::size_t index) const {
  static const SharedAutomationArea empty{};
  return index < this->area_count_ ? this->areas_[index] : empty;
}

}  // namespace radar_api_server
}  // namespace esphome
