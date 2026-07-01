#include "radar_api_server.h"

#include "http_response.h"
#include "esphome/core/log.h"

namespace esphome {
namespace radar_api_server {

static const char *const TAG = "radar_api_server";

void RadarApiServer::setup() {
  this->storage_.ensure_partition();
  std::string config;
  if (this->storage_.read_payload(RadarPayloadTarget::DEVICE_CONFIG, &config)) {
    this->device_config_cache_.update(config);
  } else {
    this->device_config_cache_.clear();
  }
  this->stats_store_.load(&this->storage_);
  this->base_->add_handler(this);
}

void RadarApiServer::dump_config() {
  ESP_LOGCONFIG(TAG, "Radar API Server:");
  ESP_LOGCONFIG(TAG, "  Dashboard: /dashboard");
  ESP_LOGCONFIG(TAG, "  Floorplan Status API: /api/floorplan/status");
  ESP_LOGCONFIG(TAG, "  Floorplan Config API: GET/POST /api/floorplan");
  ESP_LOGCONFIG(TAG, "  Floorplan Image API: GET/POST /api/floorplan/image");
  ESP_LOGCONFIG(TAG, "  Device Config Status API: /api/config/status");
  ESP_LOGCONFIG(TAG, "  Device Config API: GET/POST /api/config");
  ESP_LOGCONFIG(TAG, "  State API: GET /api/state");
  ESP_LOGCONFIG(TAG, "  Stats API: GET/POST /api/stats");
  ESP_LOGCONFIG(TAG, "  Stats Upload API: POST /api/stats/upload/start|chunk|commit");
  ESP_LOGCONFIG(TAG, "  System Status API: /api/system/status");
  ESP_LOGCONFIG(TAG, "  Control Status API: GET /api/control/status");
  ESP_LOGCONFIG(TAG, "  Control API: POST /api/control/status-led|led-duration");
  ESP_LOGCONFIG(TAG, "  Floorplan Upload API: POST /api/floorplan/upload/start|chunk|commit");
}

float RadarApiServer::get_setup_priority() const { return setup_priority::WIFI - 1.0f; }

bool RadarApiServer::canHandle(AsyncWebServerRequest *request) const {
  return this->dashboard_handler_.can_handle(request) || this->floorplan_handler_.can_handle(request) ||
         this->device_config_handler_.can_handle(request) || this->stats_handler_.can_handle(request) ||
         this->system_handler_.can_handle(request) || this->state_handler_.can_handle(request) ||
         this->control_handler_.can_handle(request);
}

void RadarApiServer::handleRequest(AsyncWebServerRequest *request) {
  if (this->dashboard_handler_.handle(request))
    return;
  if (this->floorplan_handler_.handle(request))
    return;
  if (this->device_config_handler_.handle(request))
    return;
  if (this->stats_handler_.handle(request))
    return;
  if (this->system_handler_.handle(request))
    return;
  if (this->state_handler_.handle(request))
    return;
  if (this->control_handler_.handle(request))
    return;

  http_response::send_error(request, 404, "not_found");
}

}  // namespace radar_api_server
}  // namespace esphome
