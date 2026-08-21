#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../../components/radar_api_server/presence_tracker.h"

namespace {

using esphome::radar_api_server::PresenceTracker;
using esphome::radar_api_server::PresenceTrackerInput;

bool parse_number_after_key(const std::string &line, const char *key, double &out) {
  const size_t key_pos = line.find(key);
  if (key_pos == std::string::npos)
    return false;

  const char *start = line.c_str() + key_pos + std::strlen(key);
  char *end = nullptr;
  errno = 0;
  out = std::strtod(start, &end);
  return end != start && errno != ERANGE;
}

bool extract_array_numbers(const std::string &line, const char *key, std::vector<double> &out) {
  out.clear();
  const size_t key_pos = line.find(key);
  if (key_pos == std::string::npos)
    return false;

  const size_t array_start = line.find('[', key_pos + std::strlen(key));
  if (array_start == std::string::npos)
    return false;

  int depth = 0;
  size_t array_end = std::string::npos;
  for (size_t i = array_start; i < line.size(); i++) {
    if (line[i] == '[') {
      depth++;
    } else if (line[i] == ']') {
      depth--;
      if (depth == 0) {
        array_end = i;
        break;
      }
    }
  }
  if (array_end == std::string::npos)
    return false;

  const char *cursor = line.c_str() + array_start;
  const char *end = line.c_str() + array_end + 1;
  while (cursor < end) {
    while (cursor < end && *cursor != '-' && *cursor != '+' && *cursor != '.' &&
           (*cursor < '0' || *cursor > '9')) {
      cursor++;
    }
    if (cursor >= end)
      break;

    char *next = nullptr;
    errno = 0;
    const double value = std::strtod(cursor, &next);
    if (next == cursor || errno == ERANGE)
      return false;

    out.push_back(value);
    cursor = next;
  }
  return true;
}

uint8_t tracker_state_code(const char *value) {
  const char *state = value != nullptr && value[0] != '\0' ? value : "idle";
  if (std::strcmp(state, "idle") == 0) return 0;
  if (std::strcmp(state, "tentative") == 0) return 1;
  if (std::strcmp(state, "confirmed") == 0) return 2;
  if (std::strcmp(state, "coasting") == 0) return 3;
  return 255;
}

uint8_t tracker_reason_code(const char *value) {
  const char *reason = value != nullptr && value[0] != '\0' ? value : "none";
  if (std::strcmp(reason, "none") == 0) return 0;
  if (std::strcmp(reason, "confirmed_by_hits") == 0) return 1;
  if (std::strcmp(reason, "confirmed_by_pir_hint") == 0) return 2;
  if (std::strcmp(reason, "confirmed_reacquired") == 0) return 3;
  if (std::strcmp(reason, "coasting_missed") == 0) return 4;
  if (std::strcmp(reason, "pir") == 0) return 5;
  if (std::strcmp(reason, "idle") == 0) return 6;
  if (std::strcmp(reason, "filter_blocked") == 0) return 7;
  if (std::strcmp(reason, "filter_blocked_missed") == 0) return 8;
  if (std::strcmp(reason, "tentative_new") == 0) return 9;
  if (std::strcmp(reason, "tentative_waiting_hits") == 0) return 10;
  if (std::strcmp(reason, "tentative_missed") == 0) return 11;
  if (std::strcmp(reason, "coasting_after_exit") == 0) return 12;
  if (std::strcmp(reason, "lost_after_exit") == 0) return 13;
  if (std::strcmp(reason, "lost_without_exit") == 0) return 14;
  if (std::strcmp(reason, "coasting_after_room_exit") == 0) return 15;
  if (std::strcmp(reason, "lost_after_room_exit") == 0) return 16;
  if (std::strcmp(reason, "tentative_reduced_zone") == 0) return 17;
  return 255;
}

bool parse_replay_line(const std::string &line, PresenceTrackerInput &input, uint32_t &sequence) {
  double value = 0.0;
  if (!parse_number_after_key(line, "\"q\":", value))
    return false;
  sequence = static_cast<uint32_t>(value);

  if (!parse_number_after_key(line, "\"t\":", value))
    return false;
  input.now_ms = static_cast<uint32_t>(value);

  if (!parse_number_after_key(line, "\"p\":", value))
    return false;
  input.pir_motion = static_cast<int>(value) != 0;

  if (!parse_number_after_key(line, "\"lx\":", value))
    return false;
  input.illuminance_lux = static_cast<float>(value);

  std::vector<double> filter_values;
  if (!extract_array_numbers(line, "\"f\":", filter_values) || filter_values.size() < 1)
    return false;
  input.filter_blocked = static_cast<int>(filter_values[0]) != 0;

  std::vector<double> target_values;
  if (!extract_array_numbers(line, "\"tg\":", target_values) ||
      (target_values.size() != 15 && target_values.size() != 18 && target_values.size() != 21))
    return false;

  const int tuple_size = static_cast<int>(target_values.size() / 3);
  for (int i = 0; i < 3; i++) {
    const int base = i * tuple_size;
    input.targets[i].valid = static_cast<int>(target_values[base]) != 0;
    input.targets[i].x_mm = static_cast<float>(target_values[base + 1]);
    input.targets[i].y_mm = static_cast<float>(target_values[base + 2]);
    input.targets[i].speed_cm_s = static_cast<float>(target_values[base + 3]);
    input.targets[i].distance_mm = static_cast<float>(target_values[base + 4]);
    input.targets[i].reduced_zone = tuple_size >= 6 && static_cast<int>(target_values[base + 5]) != 0;
    input.targets[i].filter_blocked = tuple_size == 7 && static_cast<int>(target_values[base + 6]) != 0;
  }

  return true;
}

void print_tracker_output(uint32_t sequence, uint32_t ms, const PresenceTracker &tracker) {
  const auto &out = tracker.output();
  std::cout << "{\"q\":" << sequence << ",\"t\":" << ms << ",\"tr\":["
            << (out.presence ? 1 : 0) << "," << (out.motion ? 1 : 0) << ","
            << static_cast<int>(tracker_state_code(out.state)) << ","
            << static_cast<int>(tracker_reason_code(out.reason)) << "," << out.track_score << ","
            << out.input_detection_count << "," << out.active_track_count << ","
            << out.tentative_track_count << "," << out.confirmed_track_count << ","
            << out.coasting_track_count << "," << out.moving_track_count << ","
            << out.still_track_count << "]}\n";
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: tracker_runner <presence-replay.ndjson>\n";
    return 2;
  }

  std::ifstream input_file(argv[1]);
  if (!input_file) {
    std::cerr << "error: failed to open " << argv[1] << "\n";
    return 1;
  }

  PresenceTracker tracker;
  std::string line;
  uint32_t line_number = 0;
  while (std::getline(input_file, line)) {
    line_number++;
    if (line.empty())
      continue;

    PresenceTrackerInput input;
    uint32_t sequence = 0;
    if (!parse_replay_line(line, input, sequence)) {
      std::cerr << "error: failed to parse replay line " << line_number << "\n";
      return 1;
    }

    tracker.update(input);
    print_tracker_output(sequence, input.now_ms, tracker);
  }

  return 0;
}
