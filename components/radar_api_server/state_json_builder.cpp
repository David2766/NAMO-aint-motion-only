#include "state_json_builder.h"

#include <cmath>
#include <cstdio>

namespace esphome {
namespace radar_api_server {

namespace {

template<typename Output> void append_json_string(Output &out, const char *value) {
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

template<typename Output> void append_json_string(Output &out, const std::string &value) {
  append_json_string(out, value.c_str());
}

template<typename Output>
void append_float(Output &out, const char *name, float value,
                  const char *format) {
  out += ",\"";
  out += name;
  out += "\":";
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char value_buf[24];
  std::snprintf(value_buf, sizeof(value_buf), format, value);
  out += value_buf;
}

const char *zone_type_name(CachedZoneType type) {
  switch (type) {
    case CachedZoneType::DETECTION:
      return "detection";
    case CachedZoneType::FILTER:
      return "filter";
    case CachedZoneType::REDUCED:
      return "reduced";
    case CachedZoneType::DISABLED:
      return "disabled";
    case CachedZoneType::EXIT:
      return "exit";
    default:
      return "detection";
  }
}

template<typename Output>
void append_target_json(Output &out, const StateJsonTarget &target) {
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
  out += ",\"fused\":";
  out += target.fused ? "true" : "false";
  out += ",\"sourceCount\":";
  out += std::to_string(target.source_count);
  out += "}";
}

template<typename Output>
void append_local_target_json(Output &out, const StateJsonLocalTarget &target) {
  out += "{\"x\":";
  out += std::to_string(static_cast<int>(std::lround(target.x)));
  out += ",\"y\":";
  out += std::to_string(static_cast<int>(std::lround(target.y)));
  out += ",\"active\":";
  out += target.active ? "true" : "false";
  out += "}";
}

template<typename Output>
void append_zone_json(Output &out, const DeviceConfigCache &config_cache,
                      int zone_index, int target_count) {
  const auto &zone = config_cache.software_zone(zone_index);
  char zone_id[16];
  std::snprintf(zone_id, sizeof(zone_id), "zone_%d", zone_index);
  const std::string &zone_name = config_cache.software_zone_name(zone_index);
  out += "{\"id\":";
  append_json_string(out, zone_id);
  out += ",\"name\":";
  append_json_string(out, zone_name.empty() ? zone_id : zone_name.c_str());
  out += ",\"type\":";
  append_json_string(out, zone_type_name(zone.type));
  out += ",\"presence\":";
  out += target_count > 0 ? "true" : "false";
  out += ",\"targetCount\":";
  out += std::to_string(target_count);
  out += "}";
}

template<typename Output>
void append_automation_zone_json(Output &out,
                                 const DeviceConfigCache &config_cache,
                                 int zone_index, int target_count,
                                 bool available) {
  const auto &zone = config_cache.software_zone(zone_index);
  char zone_id[16];
  std::snprintf(zone_id, sizeof(zone_id), "zone_%d", zone_index);
  const std::string &zone_name = config_cache.software_zone_name(zone_index);
  out += "{\"zoneId\":";
  append_json_string(out, zone_id);
  out += ",\"name\":";
  append_json_string(out, zone_name.empty() ? zone_id : zone_name.c_str());
  out += ",\"type\":";
  append_json_string(out, zone_type_name(zone.type));
  out += ",\"presence\":";
  if (available)
    out += target_count > 0 ? "true" : "false";
  else
    out += "null";
  out += ",\"targetCount\":";
  if (available)
    out += std::to_string(target_count);
  else
    out += "null";
  out += "}";
}

template<typename Output>
void append_shared_area_json(Output &out, const SharedAutomationArea &area,
                             int target_count, bool available) {
  out += "{\"areaId\":";
  append_json_string(out, area.area_id);
  out += ",\"name\":";
  append_json_string(out, area.name[0] != '\0' ? area.name : area.area_id);
  out += ",\"presence\":";
  if (available)
    out += target_count > 0 ? "true" : "false";
  else
    out += "null";
  out += ",\"targetCount\":";
  if (available)
    out += std::to_string(target_count);
  else
    out += "null";
  out += "}";
}

int count_local_area_targets(const SharedAutomationArea &area,
                             const DeviceStateJsonInput &input) {
  int count = 0;
  for (const auto &target : input.targets) {
    if (target.active && area.contains(target.x, target.y))
      ++count;
  }
  return count;
}

int count_automation_area_targets(const SharedAutomationArea &area,
                                  const FusionAutomationSnapshot *automation) {
  if (automation == nullptr)
    return 0;
  int count = 0;
  for (const auto &target : automation->fusion.targets) {
    if (target.valid && area.contains(target.x_mm, target.y_mm))
      ++count;
  }
  return count;
}

template<typename Output, typename Counter>
void append_shared_area_collection(Output &out,
                                   const SharedAutomationAreaCache *cache,
                                   bool source_available, Counter counter) {
  const bool contract_available =
      cache != nullptr && cache->configuration_valid();
  out += ",\"floorplanId\":";
  if (cache != nullptr && cache->floorplan_id()[0] != '\0')
    append_json_string(out, cache->floorplan_id());
  else
    out += "null";
  out += ",\"areasRevision\":";
  out += std::to_string(cache != nullptr ? cache->revision() : 0);
  out += ",\"areasAvailable\":";
  out += contract_available && source_available ? "true" : "false";
  out += ",\"areas\":[";
  bool first = true;
  if (cache != nullptr && contract_available) {
    for (std::size_t index = 0; index < cache->size(); ++index) {
      const auto &area = cache->area(index);
      if (!area.occupied)
        continue;
      if (!first)
        out += ",";
      append_shared_area_json(out, area, counter(area), source_available);
      first = false;
    }
  }
  out += "]";
}

template<typename Output>
void append_device_state_json(Output &out,
                              const DeviceConfigCache &config_cache,
                              const SoftwareZoneEvidence &zone_evidence,
                              const char *tracker_debug_json,
                              const DeviceStateJsonInput &input) {
  out += "{\"version\":1,\"connected\":true,\"updatedAt\":";
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
  out += ",\"fusion\":{\"buildEnabled\":";
  out += input.fusion_build_enabled ? "true" : "false";
  out += ",\"authoritative\":";
  out += input.fusion_authoritative ? "true" : "false";
  out += ",\"state\":";
  append_json_string(out, input.fusion_state);
  const auto *automation = input.automation;
  const bool automation_available =
      automation != nullptr &&
      ((automation->authoritative &&
        automation->health == FusionAutomationHealth::HEALTHY) ||
       (!automation->authoritative && automation->continuity_held &&
        automation->health == FusionAutomationHealth::RECOVERING &&
        FusionAutomationState::continuity_remaining_ms(*automation,
                                                        input.now_ms) > 0));
  if (automation != nullptr) {
    out += ",\"source\":";
    append_json_string(
        out, fusion_automation_source_to_string(automation->source));
    out += ",\"groupTerm\":";
    out += std::to_string(automation->group_term);
    out += ",\"sequence\":";
    out += std::to_string(automation->sequence);
    out += ",\"sourceSequence\":";
    out += std::to_string(automation->source_sequence);
  }
  out += "}";
  out += ",\"local\":{\"presence\":";
  out += input.local_presence ? "true" : "false";
  out += ",\"motion\":";
  out += input.local_motion ? "true" : "false";
  out += ",\"targetCount\":";
  out += std::to_string(input.local_target_count);
  out += ",\"targets\":[";
  append_local_target_json(out, input.local_targets[0]);
  out += ",";
  append_local_target_json(out, input.local_targets[1]);
  out += ",";
  append_local_target_json(out, input.local_targets[2]);
  out += "]";
  out += "}";
  out += ",\"automation\":{\"schemaVersion\":1,\"complete\":true,\"health\":";
  append_json_string(
      out, automation != nullptr
               ? fusion_automation_health_to_string(automation->health)
               : "disabled");
  out += ",\"available\":";
  out += automation_available ? "true" : "false";
  out += ",\"continuityHeld\":";
  out += automation != nullptr && automation->continuity_held ? "true"
                                                               : "false";
  out += ",\"holdAgeMs\":";
  if (automation != nullptr && automation->continuity_held)
    out += std::to_string(FusionAutomationState::continuity_age_ms(
        *automation, input.now_ms));
  else
    out += "null";
  out += ",\"holdRemainingMs\":";
  if (automation != nullptr && automation->continuity_held)
    out += std::to_string(FusionAutomationState::continuity_remaining_ms(
        *automation, input.now_ms));
  else
    out += "null";
  out += ",\"continuitySourceLeaderNodeId\":";
  if (automation != nullptr && automation->continuity_held &&
      automation->continuity_source_leader_node_id[0] != '\0')
    append_json_string(out,
                       automation->continuity_source_leader_node_id);
  else
    out += "null";
  out += ",\"continuitySourceGroupTerm\":";
  if (automation != nullptr && automation->continuity_held)
    out += std::to_string(automation->continuity_source_group_term);
  else
    out += "null";
  out += ",\"siteId\":";
  if (automation != nullptr && automation->site_id[0] != '\0')
    append_json_string(out, automation->site_id);
  else
    out += "null";
  out += ",\"spaceId\":";
  if (automation != nullptr && automation->space_id[0] != '\0')
    append_json_string(out, automation->space_id);
  else
    out += "null";
  out += ",\"groupId\":";
  if (automation != nullptr && automation->group_id[0] != '\0')
    append_json_string(out, automation->group_id);
  else
    out += "null";
  out += ",\"leaderNodeId\":";
  if (automation != nullptr && automation->leader_node_id[0] != '\0')
    append_json_string(out, automation->leader_node_id);
  else
    out += "null";
  out += ",\"bootId\":";
  if (automation != nullptr && automation->boot_id[0] != '\0')
    append_json_string(out, automation->boot_id);
  else
    out += "null";
  out += ",\"source\":";
  append_json_string(
      out, automation != nullptr
               ? fusion_automation_source_to_string(automation->source)
               : "none");
  out += ",\"groupTerm\":";
  out += std::to_string(automation != nullptr ? automation->group_term : 0);
  out += ",\"sequence\":";
  out += std::to_string(automation != nullptr ? automation->sequence : 0);
  out += ",\"sourceSequence\":";
  out += std::to_string(
      automation != nullptr ? automation->source_sequence : 0);
  out += ",\"ageMs\":";
  if (automation != nullptr && automation->updated_at_ms != 0 &&
      input.now_ms >= automation->updated_at_ms)
    out += std::to_string(input.now_ms - automation->updated_at_ms);
  else
    out += "null";
  out += ",\"presence\":";
  if (automation_available)
    out += automation->presence ? "true" : "false";
  else
    out += "null";
  out += ",\"motion\":";
  if (automation_available)
    out += automation->motion ? "true" : "false";
  else
    out += "null";
  out += ",\"targetCount\":";
  if (automation_available)
    out += std::to_string(automation->fusion.target_count);
  else
    out += "null";
  append_shared_area_collection(
      out, input.shared_automation_areas, automation_available,
      [automation](const SharedAutomationArea &area) {
        return count_automation_area_targets(area, automation);
      });
  out += ",\"zones\":[";
  bool first_automation_zone = true;
  for (int zone_index = 1; zone_index <= MAX_SOFTWARE_ZONES; zone_index++) {
    if (!config_cache.software_zone(zone_index).valid)
      continue;
    if (!first_automation_zone)
      out += ",";
    append_automation_zone_json(
        out, config_cache, zone_index,
        zone_evidence.state_zone_counts[zone_index - 1],
        automation_available);
    first_automation_zone = false;
  }
  out += "]}";
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
  for (int zone_index = 1; zone_index <= MAX_SOFTWARE_ZONES; zone_index++) {
    if (!config_cache.software_zone(zone_index).valid)
      continue;
    if (!first_zone)
      out += ",";
    append_zone_json(out, config_cache, zone_index, zone_evidence.state_zone_counts[zone_index - 1]);
    first_zone = false;
  }
  out += "]";
  append_shared_area_collection(
      out, input.shared_automation_areas,
      input.shared_automation_areas != nullptr &&
          input.shared_automation_areas->configuration_valid(),
      [&input](const SharedAutomationArea &area) {
        return count_local_area_targets(area, input);
      });
  out += "}";
}

}  // namespace

std::string build_device_state_json(
    const DeviceConfigCache &config_cache,
    const SoftwareZoneEvidence &zone_evidence,
    const std::string &tracker_debug_json,
    const DeviceStateJsonInput &input) {
  std::string out;
  out.reserve(4096);
  const auto config = config_cache.read();
  append_device_state_json(out, config.get(), zone_evidence,
                           tracker_debug_json.c_str(), input);
  return out;
}

bool build_device_state_json(FixedTextBuffer &output,
                             const DeviceConfigCache &config_cache,
                             const SoftwareZoneEvidence &zone_evidence,
                             const char *tracker_debug_json,
                             const DeviceStateJsonInput &input) {
  output.clear();
  const auto config = config_cache.read();
  append_device_state_json(output, config.get(), zone_evidence,
                           tracker_debug_json, input);
  return output.ok();
}

}  // namespace radar_api_server
}  // namespace esphome
