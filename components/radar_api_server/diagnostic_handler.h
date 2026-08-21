#pragma once

#include "diagnostic_log.h"
#include "presence_replay_log.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace radar_api_server {

class DiagnosticHandler {
 public:
  DiagnosticHandler(DiagnosticLog *diagnostic_log, PresenceReplayLog *presence_replay_log)
      : diagnostic_log_(diagnostic_log), presence_replay_log_(presence_replay_log) {}

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 private:
  DiagnosticLog *diagnostic_log_;
  PresenceReplayLog *presence_replay_log_;

  void handle_events_json_(AsyncWebServerRequest *request);
  void handle_events_text_(AsyncWebServerRequest *request);
  void handle_replay_ndjson_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
