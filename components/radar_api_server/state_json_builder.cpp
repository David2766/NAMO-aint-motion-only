#include "state_json_builder.h"

#include <cmath>
#include <cstdio>

namespace esphome {
namespace radar_api_server {

namespace {

void append_json_string(std::string &out, const char *value) {
  out.push_back('"');
  const char *safe = value != nullptr ? value : "";
  for (const char *p = safe; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(ch));
    } else if (ch < 0x20) {
      char buf[7];
      std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
      out += buf;
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  out.push_back('"');
}

void append_json_string(std::string &out, const std::string &value) {
  append_json_string(out, value.c_str());
}

void append_float(std::string &out, const char *name, float value, const char *format) {
  out += ",\"";
  out += name;
  out += "\":";
  if (std::isnan(value)) {
    out += "null";
    return;
  }
  char value_buf[24];
  std::snprintf(value_buf, sizeof(value_buf), format, value);
  out += value_buf;
}

bool zone_config_valid(const std::string &value, int zone_index) {
  const std::string needle = std::string("\"id\":\"zone_") + std::to_string(zone_index) + "\"";
  return !value.empty() && value != "__EMPTY__" && value.find(needle) != std::string::npos &&
         value.find("\"points\"") != std::string::npos;
}

std::string json_string_value(const std::string &json, const char *key, const char *fallback) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t value_start = json.find(needle);
  if (value_start == std::string::npos)
    return fallback;

  size_t cursor = value_start + needle.size();
  bool escaped = false;
  while (cursor < json.size()) {
    const char current = json[cursor];
    if (escaped) {
      escaped = false;
    } else if (current == '\\') {
      escaped = true;
    } else if (current == '"') {
      return json.substr(value_start + needle.size(), cursor - (value_start + needle.size()));
    }
    cursor++;
  }
  return fallback;
}

void append_target_json(std::string &out, const StateJsonTarget &target) {
  out += "{\"id\":";
  append_json_string(out, target.id);
  out += ",\"name\":";
  append_json_string(out, target.label);
  out += ",\"color\":";
  append_json_string(out, target.color);
  out += ",\"x\":";
  out += std::to_string(static_cast<int>(std::lround(target.x)));
  out += ",\"y\":";
  out += std::to_string(static_cast<int>(std::lround(target.y)));
  out += ",\"active\":";
  out += target.active ? "true" : "false";
  out += "}";
}

void append_zone_json(std::string &out, const DeviceConfigCache &config_cache, int zone_index, int target_count) {
  const std::string zone_config = config_cache.software_zone_config(zone_index);
  const std::string zone_id = std::string("zone_") + std::to_string(zone_index);
  const std::string zone_name = json_string_value(zone_config, "name", "");
  const std::string zone_type = json_string_value(zone_config, "type", "detection");
  out += "{\"id\":";
  append_json_string(out, zone_id);
  out += ",\"name\":";
  append_json_string(out, zone_name.empty() ? zone_id : zone_name);
  out += ",\"type\":";
  append_json_string(out, zone_type);
  out += ",\"presence\":";
  out += target_count > 0 ? "true" : "false";
  out += ",\"targetCount\":";
  out += std::to_string(target_count);
  out += "}";
}

}  // namespace

std::string build_device_state_json(const DeviceConfigCache &config_cache, const SoftwareZoneEvidence &zone_evidence,
                                    const std::string &tracker_debug_json, const DeviceStateJsonInput &input) {
  std::string out = "{\"version\":1,\"connected\":true,\"updatedAt\":";
  out += std::to_string(input.now_ms);
  out += ",\"pirMotion\":";
  out += input.pir_motion ? "true" : "false";
  out += ",\"pirMotionEffective\":";
  out += input.pir_motion_effective ? "true" : "false";
  out += ",\"filterBlocked\":";
  out += input.filter_blocked ? "true" : "false";
  out += ",\"presence\":";
  out += input.presence ? "true" : "false";
  out += ",\"motion\":";
  out += input.motion ? "true" : "false";
  out += ",\"targetCount\":";
  out += std::to_string(input.target_count);
  out += ",\"movingTargetCount\":";
  out += std::to_string(input.moving_target_count);
  out += ",\"stillTargetCount\":";
  out += std::to_string(input.still_target_count);
  append_float(out, "temperatureC", input.temperature_c, "%.1f");
  append_float(out, "humidityPercent", input.humidity_percent, "%.1f");
  append_float(out, "illuminanceLux", input.illuminance_lux, "%.0f");

  out += ",\"debug\":{\"presenceReason\":";
  append_json_string(out, input.presence_reason);
  out += ",\"presenceOffReason\":";
  append_json_string(out, input.presence_off_reason);
  out += ",\"motionReason\":";
  append_json_string(out, input.motion_reason);
  out += ",\"lastPresenceDropMs\":";
  out += std::to_string(input.last_presence_drop_ms);
  out += ",\"lastValidTargetAgeMs\":";
  if (input.has_last_valid_target_age) {
    out += std::to_string(input.last_valid_target_age_ms);
  } else {
    out += "null";
  }
  out += ",\"emptySamplesConsecutive\":";
  out += std::to_string(input.empty_samples_consecutive);
  out += ",\"shortPresenceDropCount\":";
  out += std::to_string(input.short_presence_drop_count);
  out += ",\"longPresenceDropCount\":";
  out += std::to_string(input.long_presence_drop_count);

  out += ",\"presenceEvidence\":{\"pir\":";
  out += input.pir_evidence ? "true" : "false";
  out += ",\"tracker\":";
  out += input.tracker_evidence ? "true" : "false";
  out += ",\"staticAssist\":";
  out += input.static_assist_active ? "true" : "false";
  out += "}";

  out += ",\"still\":{\"state\":";
  append_json_string(out, input.still_state);
  out += ",\"reason\":";
  append_json_string(out, input.still_reason);
  out += ",\"confidence\":";
  out += std::to_string(input.still_confidence);
  out += ",\"holdActive\":";
  out += input.still_hold_active ? "true" : "false";
  out += ",\"lastSeenAgeMs\":";
  if (input.has_still_last_seen_age) {
    out += std::to_string(input.still_last_seen_age_ms);
  } else {
    out += "null";
  }
  out += ",\"anchor\":";
  if (!input.still_anchor_valid) {
    out += "null";
  } else {
    out += "{\"x\":";
    out += std::to_string(static_cast<int>(std::lround(input.still_anchor_x)));
    out += ",\"y\":";
    out += std::to_string(static_cast<int>(std::lround(input.still_anchor_y)));
    out += "}";
  }

  out += "},\"range\":{\"reason\":";
  append_json_string(out, input.range_reason);
  out += ",\"suspectTargetCount\":";
  out += std::to_string(input.range_suspect_count);
  out += ",\"outOfRangeTargetCount\":";
  out += std::to_string(input.range_out_of_range_count);
  out += ",\"remoteCandidateCount\":";
  out += std::to_string(input.range_remote_candidate_count);
  out += "}";

  out += ",\"staticRadar\":{\"available\":";
  out += input.static_radar.available ? "true" : "false";
  out += ",\"presence\":";
  out += input.static_radar.presence ? "true" : "false";
  out += ",\"moving\":";
  out += input.static_radar.moving ? "true" : "false";
  out += ",\"still\":";
  out += input.static_radar.still ? "true" : "false";
  append_float(out, "detectionDistanceMm", input.static_radar.detection_distance_mm, "%.0f");
  append_float(out, "movingDistanceMm", input.static_radar.moving_distance_mm, "%.0f");
  append_float(out, "stillDistanceMm", input.static_radar.still_distance_mm, "%.0f");
  append_float(out, "movingEnergy", input.static_radar.moving_energy, "%.0f");
  append_float(out, "stillEnergy", input.static_radar.still_energy, "%.0f");
  out += ",\"reason\":";
  append_json_string(out, input.static_radar_reason);
  out += ",\"assist\":{\"armed\":";
  out += input.static_assist_armed ? "true" : "false";
  out += ",\"active\":";
  out += input.static_assist_active ? "true" : "false";
  out += ",\"armPending\":";
  out += input.static_assist_arm_pending ? "true" : "false";
  out += ",\"armElapsedMs\":";
  out += std::to_string(input.static_assist_arm_elapsed_ms);
  out += ",\"exitVeto\":";
  out += input.static_assist_exit_veto ? "true" : "false";
  out += ",\"heldTarget\":";
  if (!input.static_assist_held_target.valid) {
    out += "null";
  } else {
    out += "{\"id\":\"target_1\",\"x\":";
    out += std::to_string(static_cast<int>(std::lround(input.static_assist_held_target.x_mm)));
    out += ",\"y\":";
    out += std::to_string(static_cast<int>(std::lround(input.static_assist_held_target.y_mm)));
    out += ",\"lastDistanceMm\":";
    out += std::to_string(static_cast<int>(std::lround(input.static_assist_held_target.last_distance_mm)));
    out += ",\"staticDistanceMm\":";
    if (input.static_assist_held_target.distance_comparable) {
      out += std::to_string(static_cast<int>(std::lround(input.static_assist_held_target.static_distance_mm)));
    } else {
      out += "null";
    }
    out += ",\"distanceDeltaMm\":";
    if (input.static_assist_held_target.distance_comparable) {
      out += std::to_string(static_cast<int>(std::lround(input.static_assist_held_target.distance_delta_mm)));
    } else {
      out += "null";
    }
    out += ",\"distanceMatched\":";
    out += input.static_assist_held_target.distance_matched ? "true" : "false";
    out += "}";
  }
  out += "}";
  out += "}";

  out += ",\"room\":{\"configured\":";
  out += zone_evidence.room_context_configured ? "true" : "false";
  out += ",\"boundaryPointCount\":";
  out += std::to_string(zone_evidence.room_boundary_point_count);
  out += ",\"insideTargetCount\":";
  out += std::to_string(zone_evidence.room_target_inside_count);
  out += ",\"outsideTargetCount\":";
  out += std::to_string(zone_evidence.room_target_outside_count);
  out += "}";

  out += ",\"exit\":{\"active\":";
  out += zone_evidence.exit_zone_active ? "true" : "false";
  out += ",\"zoneCount\":";
  out += std::to_string(zone_evidence.exit_zone_count);
  out += ",\"zoneMask\":";
  out += std::to_string(zone_evidence.exit_zone_mask);
  out += ",\"targetCount\":";
  out += std::to_string(zone_evidence.exit_target_count);
  out += ",\"lastSeenAgeMs\":";
  if (zone_evidence.exit_last_seen_age_ms < 0) {
    out += "null";
  } else {
    out += std::to_string(zone_evidence.exit_last_seen_age_ms);
  }
  out += "}";
  out += ",\"tracker\":";
  out += tracker_debug_json;
  out += "}";

  out += ",\"targets\":[";
  append_target_json(out, input.targets[0]);
  out += ",";
  append_target_json(out, input.targets[1]);
  out += ",";
  append_target_json(out, input.targets[2]);
  out += "],\"zones\":[";
  bool first_zone = true;
  for (int zone_index = 1; zone_index <= 6; zone_index++) {
    if (!zone_config_valid(config_cache.software_zone_config(zone_index), zone_index))
      continue;
    if (!first_zone)
      out += ",";
    append_zone_json(out, config_cache, zone_index, zone_evidence.state_zone_counts[zone_index - 1]);
    first_zone = false;
  }
  out += "]}";
  return out;
}

}  // namespace radar_api_server
}  // namespace esphome
