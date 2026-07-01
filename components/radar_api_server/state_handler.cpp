#include "state_handler.h"

namespace esphome {
namespace radar_api_server {

bool StateHandler::can_handle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);
  return url == "/api/state";
}

bool StateHandler::handle(AsyncWebServerRequest *request) {
  if (!this->can_handle(request))
    return false;

  this->handle_get_state_(request);
  return true;
}

void StateHandler::handle_get_state_(AsyncWebServerRequest *request) {
  if (this->state_json_ == nullptr || this->state_json_->empty()) {
    request->send(200, "application/json",
                  R"({"version":1,"connected":false,"updatedAt":0,"pirMotion":false,"pirMotionEffective":false,"filterBlocked":false,"presence":false,"motion":false,"targetCount":0,"movingTargetCount":0,"stillTargetCount":0,"temperatureC":null,"humidityPercent":null,"illuminanceLux":null,"targets":[]})");
    return;
  }

  auto *response = request->beginResponse(200, "application/json", *this->state_json_);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

}  // namespace radar_api_server
}  // namespace esphome
