#pragma once

#include "device_config_cache.h"
#include "radar_storage.h"
#include "stats_store.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

class RadarApiServer;
struct SystemStatusWorkspace;

class SystemHandler {
 public:
  SystemHandler(RadarStorage *storage, DeviceConfigCache *device_config_cache, StatsStore *stats_store,
                RadarApiServer *server)
      : storage_(storage), device_config_cache_(device_config_cache), stats_store_(stats_store), server_(server) {}
  ~SystemHandler();

  void setup();
  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 protected:
  struct StaticResourceInfo {
    uint32_t flash_total_bytes{0};
    uint32_t firmware_used_bytes{0};
    uint32_t firmware_slot_bytes{0};
    uint32_t ota_slot_bytes{0};
  };

  RadarStorage *storage_;
  DeviceConfigCache *device_config_cache_;
  StatsStore *stats_store_;
  RadarApiServer *server_;
  SystemStatusWorkspace *status_workspace_{nullptr};
  bool status_workspace_in_psram_{false};
  StaticResourceInfo static_resource_info_{};

  void handle_status_(AsyncWebServerRequest *request);
  void handle_api_key_(AsyncWebServerRequest *request);
  void handle_api_key_rotate_(AsyncWebServerRequest *request);
  void handle_ha_setup_handoff_(AsyncWebServerRequest *request);
  void handle_reboot_(AsyncWebServerRequest *request);
  void handle_reset_(AsyncWebServerRequest *request);
  bool request_bool_(AsyncWebServerRequest *request, const char *key, bool default_value) const;
  bool reset_device_config_(bool reset_space, bool reset_detection) const;
  bool reset_stats_() const;
};

}  // namespace radar_api_server
}  // namespace esphome
