#pragma once

#include "radar_storage.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <string>

namespace esphome {
namespace radar_api_server {

class RadarApiServer;

class FloorplanHandler {
 public:
  FloorplanHandler(RadarApiServer *server, RadarStorage *storage)
      : server_(server), storage_(storage) {}

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 private:
  RadarApiServer *server_;
  RadarStorage *storage_;

  void handle_status_(AsyncWebServerRequest *request);
  void handle_get_payload_(AsyncWebServerRequest *request, RadarPayloadTarget target, const char *content_type,
                           const char *not_found_error, const char *read_error);
  void handle_upload_start_(AsyncWebServerRequest *request);
  void handle_upload_chunk_(AsyncWebServerRequest *request);
  void handle_upload_commit_(AsyncWebServerRequest *request);
  void handle_patch_radar_(AsyncWebServerRequest *request);
  void handle_patch_room_name_(AsyncWebServerRequest *request);
  void handle_patch_occlusion_(AsyncWebServerRequest *request);
  void handle_patch_objects_(AsyncWebServerRequest *request);
  void handle_delete_storage_(AsyncWebServerRequest *request);

  bool parse_floorplan_target_(AsyncWebServerRequest *request, RadarPayloadTarget *target) const;
  bool allow_shared_write_(AsyncWebServerRequest *request) const;
  bool parse_upload_session_(AsyncWebServerRequest *request, uint32_t *session_id) const;
  bool decode_hex_(const std::string &hex, std::string *out) const;
  bool patch_floorplan_member_(const char *key, const std::string &value, std::string *out) const;
  bool patch_floorplan_room_name_(const std::string &room_id, const std::string &name, std::string *out) const;
  bool write_floorplan_document_(const std::string &document) const;
};

}  // namespace radar_api_server
}  // namespace esphome
