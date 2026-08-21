#pragma once

#include "radar_storage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct httpd_req;

namespace esphome {
namespace radar_api_server {

class StatsStore {
 public:
  struct Totals {
    int day{0};
    int f{0};
    int r{0};
    int fz[4]{0, 0, 0, 0};
    int rz[4]{0, 0, 0, 0};
    int sz[6]{0, 0, 0, 0, 0, 0};
  };

  struct ResponseWorkspace;

  StatsStore() = default;
  ~StatsStore();
  StatsStore(const StatsStore &) = delete;
  StatsStore &operator=(const StatsStore &) = delete;
  StatsStore(StatsStore &&) = delete;
  StatsStore &operator=(StatsStore &&) = delete;

  bool setup();
  void load(RadarStorage *storage);

  void update_today(const std::string &today_json);
  void record_heatmap_hit(float tx, float ty);
  bool finish_day(const std::string &finished_day_json, const std::string &new_today_json);
  bool reset_today(const std::string &new_today_json);
  void clear();

  bool stream_json(httpd_req *request) const;

 protected:
  static constexpr size_t MAX_DAILY_ENTRIES = 30;
  using HeatmapCells = std::array<uint16_t, 858>;

  static int parse_int_after_(const std::string &json, const char *key);
  static void parse_int_array_(const std::string &json, const char *key, int *out, size_t count);
  static Totals parse_totals_(const std::string &json);
  static int day_serial_(int day_key);
  static void add_totals_(Totals *target, const Totals &source);
  static std::string default_heatmap_rle_();
  static std::string json_string_(const std::string &value);
  static void decode_heatmap_rle_(const std::string &rle,
                                  HeatmapCells *cells);

  std::string compact_today_heatmap_rle_() const;
  bool ensure_workspaces_();
  void trim_daily_();
  void trim_heatmap_daily_();
  bool load_from_storage_(RadarStorage *storage);
  bool write_json_(
      bool (*write)(void *context, const char *data, size_t size),
      void *context, ResponseWorkspace *workspace) const;
  bool persist_() const;

  RadarStorage *storage_{nullptr};
  std::string today_json_;
  std::vector<std::string> daily_;
  HeatmapCells *today_heatmap_cells_{nullptr};
  bool today_heatmap_cells_in_psram_{false};
  ResponseWorkspace *response_workspace_{nullptr};
  bool response_workspace_in_psram_{false};
  ResponseWorkspace *persist_workspace_{nullptr};
  bool persist_workspace_in_psram_{false};
  std::vector<std::string> heatmap_daily_;
};

}  // namespace radar_api_server
}  // namespace esphome
