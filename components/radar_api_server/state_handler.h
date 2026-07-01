#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

#include <string>

namespace esphome {
namespace radar_api_server {

class StateHandler {
 public:
  explicit StateHandler(const std::string *state_json) : state_json_(state_json) {}

  bool can_handle(AsyncWebServerRequest *request) const;
  bool handle(AsyncWebServerRequest *request);

 private:
  const std::string *state_json_;

  void handle_get_state_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
