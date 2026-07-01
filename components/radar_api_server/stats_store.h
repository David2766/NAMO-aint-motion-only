#pragma once

#include "radar_storage.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace radar_api_server {

class StatsStore {
 public:
  void load(RadarStorage *storage);

  void update_today(const std::string &today_json);
  void record_heatmap_hit(float tx, float ty);
  bool finish_day(const std::string &finished_day_json, const std::string &new_today_json);
  bool replace_json(const std::string &json);

  std::string current_json() const;

 protected:
  struct Totals {
    int day{0};
    int f{0};
    int r{0};
    int fz[4]{0, 0, 0, 0};
    int rz[4]{0, 0, 0, 0};
    int sz[6]{0, 0, 0, 0, 0, 0};
  };

  static constexpr size_t MAX_DAILY_ENTRIES = 30;

  static std::vector<std::string> extract_object_array_(const std::string &json, const char *key);
  static std::string extract_object_(const std::string &json, const char *key);
  static std::string extract_string_(const std::string &json, const char *key);
  static int parse_int_after_(const std::string &json, const char *key);
  static void parse_int_array_(const std::string &json, const char *key, int *out, size_t count);
  static Totals parse_totals_(const std::string &json);
  static int day_serial_(int day_key);
  static void add_totals_(Totals *target, const Totals &source);
  static std::string totals_json_(const Totals &totals, int days);
  static std::string default_heatmap_rle_();
  static std::string json_string_(const std::string &value);
  static void decode_heatmap_rle_(const std::string &rle, std::array<uint16_t, 858> *cells);

  std::string compact_today_heatmap_rle_() const;
  void trim_daily_();
  void trim_heatmap_daily_();
  bool persist_() const;

  RadarStorage *storage_{nullptr};
  std::string today_json_;
  std::vector<std::string> daily_;
  std::array<uint16_t, 858> today_heatmap_cells_{};
  std::vector<std::string> heatmap_daily_;
};

}  // namespace radar_api_server
}  // namespace esphome
