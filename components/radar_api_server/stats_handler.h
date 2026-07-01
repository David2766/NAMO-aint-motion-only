#pragma once

#include "stats_store.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class StatsHandler {
 public:
  StatsHandler(StatsStore *stats_store, RadarStorage *storage) : stats_store_(stats_store), storage_(storage) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 private:
  StatsStore *stats_store_;
  RadarStorage *storage_;

  void handle_get_stats_(AsyncWebServerRequest *request);
  void handle_post_stats_(AsyncWebServerRequest *request);
  void handle_upload_start_(AsyncWebServerRequest *request);
  void handle_upload_chunk_(AsyncWebServerRequest *request);
  void handle_upload_commit_(AsyncWebServerRequest *request);

  bool decode_hex_(const std::string &hex, std::string *out) const;
};

}  // namespace radar_api_server
}  // namespace esphome
