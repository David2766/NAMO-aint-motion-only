#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class RadarApiServer;
enum class ParticipantEnrollmentResult : uint8_t;
struct PlatformResponseWorkspace;

class PlatformHandler {
 public:
  explicit PlatformHandler(RadarApiServer *server) : server_(server) {}
  ~PlatformHandler();

  void setup();
  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 protected:
  RadarApiServer *server_;
  PlatformResponseWorkspace *response_workspace_{nullptr};

  void handle_descriptor_(AsyncWebServerRequest *request);
  void handle_discovery_get_(AsyncWebServerRequest *request);
  void handle_discovery_post_(AsyncWebServerRequest *request);
  void handle_participants_get_(AsyncWebServerRequest *request);
  void handle_participants_post_(AsyncWebServerRequest *request);
  void handle_groups_get_(AsyncWebServerRequest *request);
  void handle_site_configuration_get_(AsyncWebServerRequest *request);
  void handle_site_configuration_post_(AsyncWebServerRequest *request);
  bool execute_participant_action_(AsyncWebServerRequest *request, const char *action,
                                   ParticipantEnrollmentResult *result);
  bool execute_open_action_(AsyncWebServerRequest *request, ParticipantEnrollmentResult *result);
  bool execute_stage_action_(AsyncWebServerRequest *request, ParticipantEnrollmentResult *result);
  bool execute_configure_action_(AsyncWebServerRequest *request, const char *participant_id,
                                 ParticipantEnrollmentResult *result,
                                 bool preserve_active = false);
  bool execute_existing_participant_action_(AsyncWebServerRequest *request, const char *action,
                                            const char *participant_id,
                                            ParticipantEnrollmentResult *result);
  void send_participants_(AsyncWebServerRequest *request, const char *action = nullptr);
};

}  // namespace radar_api_server
}  // namespace esphome
