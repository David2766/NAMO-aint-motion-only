#pragma once

#include "radar_storage.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <string>

namespace esphome {
namespace radar_api_server {

class FloorplanHandler {
 public:
  explicit FloorplanHandler(RadarStorage *storage) : storage_(storage) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 private:
  RadarStorage *storage_;

  void handle_status_(AsyncWebServerRequest *request);
  void handle_get_payload_(AsyncWebServerRequest *request, RadarPayloadTarget target, const char *content_type,
                           const char *not_found_error, const char *read_error);
  void handle_post_payload_(AsyncWebServerRequest *request, RadarPayloadTarget target, const char *write_error);
  void handle_post_image_(AsyncWebServerRequest *request);
  void handle_upload_start_(AsyncWebServerRequest *request);
  void handle_upload_chunk_(AsyncWebServerRequest *request);
  void handle_upload_commit_(AsyncWebServerRequest *request);
  void handle_delete_storage_(AsyncWebServerRequest *request);

  bool parse_floorplan_target_(AsyncWebServerRequest *request, RadarPayloadTarget *target) const;
  bool decode_hex_(const std::string &hex, std::string *out) const;
};

}  // namespace radar_api_server
}  // namespace esphome
