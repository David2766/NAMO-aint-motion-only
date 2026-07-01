#include "system_handler.h"

#include "dashboard_assets.h"

#include "esphome/core/application.h"

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <cstdio>
#include <string>

namespace esphome {
namespace radar_api_server {

namespace {

std::string json_escape(const char *value) {
  std::string out;
  if (value == nullptr) return out;
  for (const char *p = value; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(ch));
    } else if (ch < 0x20) {
      char buf[7];
      std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
      out += buf;
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  return out;
}

}  // namespace

bool SystemHandler::can_handle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);
  return request->method() == HTTP_GET && url == "/api/system/status";
}

bool SystemHandler::handle(AsyncWebServerRequest *request) {
  if (!this->can_handle(request)) return false;
  this->handle_status_(request);
  return true;
}

void SystemHandler::handle_status_(AsyncWebServerRequest *request) {
  const auto storage_status = this->storage_->status();

  wifi_ap_record_t ap_info{};
  const bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
  const std::string ssid = wifi_connected ? json_escape(reinterpret_cast<const char *>(ap_info.ssid)) : "";
  const int rssi = wifi_connected ? static_cast<int>(ap_info.rssi) : 0;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_buf[18];
  char device_suffix[7];
  std::snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
  std::snprintf(device_suffix, sizeof(device_suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  char device_name_suffix[7];
  std::snprintf(device_name_suffix, sizeof(device_name_suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  const std::string device_id = std::string("presence-sensor-") + device_suffix;
  const std::string device_name = std::string("Presence Sensor ") + device_name_suffix;
  const std::string host_name = App.get_name() + ".local";
  char ip_buf[16] = "";
  auto *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info{};
  if (sta_netif != nullptr && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    std::snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
  }

  const uint32_t free_heap = esp_get_free_heap_size();
  const uint32_t min_free_heap = esp_get_minimum_free_heap_size();
  const size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const int64_t uptime_seconds = esp_timer_get_time() / 1000000LL;
  uint32_t flash_total = 0;
  if (esp_flash_get_size(nullptr, &flash_total) != ESP_OK) {
    flash_total = 0;
  }

  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  const esp_partition_t *next_ota_partition = esp_ota_get_next_update_partition(running_partition);
  uint32_t firmware_slot_size = running_partition != nullptr ? running_partition->size : 0;
  uint32_t ota_slot_size = next_ota_partition != nullptr ? next_ota_partition->size : 0;
  uint32_t firmware_used_size = 0;
  if (running_partition != nullptr) {
    esp_partition_pos_t running_pos{};
    running_pos.offset = running_partition->address;
    running_pos.size = running_partition->size;
    esp_image_metadata_t metadata{};
    if (esp_image_get_metadata(&running_pos, &metadata) == ESP_OK) {
      firmware_used_size = metadata.image_len;
    }
  }

  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
        "{\"ok\":true,"
        "\"device\":{\"type\":\"presence-sensor\",\"id\":\"%s\",\"name\":\"%s\",\"dashboardPath\":\"/dashboard\"},"
        "\"network\":{\"ip\":\"%s\",\"mac\":\"%s\",\"host\":\"%s\"},"
        "\"firmware\":{\"version\":\"%s\",\"buildTime\":\"%s %s\",\"uptimeSeconds\":%lld},"
        "\"dashboard\":{\"version\":\"%s\",\"gzipBytes\":%u},"
        "\"schema\":{\"config\":%d,\"floorplan\":%d,\"stats\":%d},"
        "\"memory\":{\"freeHeap\":%u,\"minFreeHeap\":%u,"
        "\"internalTotalBytes\":%u,\"internalFreeBytes\":%u,\"internalMinFreeBytes\":%u,"
        "\"psramTotal\":%u,\"psramFree\":%u,\"externalTotalBytes\":%u,\"externalFreeBytes\":%u},"
        "\"flash\":{\"totalBytes\":%u,\"firmwareUsedBytes\":%u,\"firmwareSlotBytes\":%u,"
        "\"otaSlotBytes\":%u,\"storageUsedBytes\":%u,\"storageTotalBytes\":%u},"
        "\"storage\":{\"ok\":%s,\"partition\":\"%s\",\"totalBytes\":%u,\"usedBytes\":%u,"
        "\"floorplanConfigBytes\":%u,\"floorplanImageBytes\":%u,\"deviceConfigBytes\":%u,\"statsBytes\":%u,"
        "\"maxPayloadBytes\":%u},"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"bluetooth\":{\"enabled\":true,\"connected\":false}}",
        device_id.c_str(), device_name.c_str(), ip_buf, mac_buf, host_name.c_str(), dashboard_assets::FIRMWARE_VERSION,
        __DATE__, __TIME__, static_cast<long long>(uptime_seconds),
        dashboard_assets::DASHBOARD_VERSION, static_cast<unsigned>(dashboard_assets::DASHBOARD_TOTAL_GZ_SIZE),
        dashboard_assets::CONFIG_SCHEMA_VERSION, dashboard_assets::FLOORPLAN_SCHEMA_VERSION,
        dashboard_assets::STATS_SCHEMA_VERSION, static_cast<unsigned>(free_heap), static_cast<unsigned>(min_free_heap),
        static_cast<unsigned>(internal_total), static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_min_free),
        static_cast<unsigned>(psram_total), static_cast<unsigned>(psram_free), static_cast<unsigned>(psram_total),
        static_cast<unsigned>(psram_free), static_cast<unsigned>(flash_total),
        static_cast<unsigned>(firmware_used_size), static_cast<unsigned>(firmware_slot_size),
        static_cast<unsigned>(ota_slot_size), static_cast<unsigned>(storage_status.used_bytes),
        static_cast<unsigned>(storage_status.total_bytes), storage_status.ok ? "true" : "false",
        storage_status.partition_label, static_cast<unsigned>(storage_status.total_bytes),
        static_cast<unsigned>(storage_status.used_bytes), static_cast<unsigned>(storage_status.header.config_size),
        static_cast<unsigned>(storage_status.header.image_size), static_cast<unsigned>(storage_status.header.reserved[0]),
        static_cast<unsigned>(storage_status.header.reserved[1]),
        static_cast<unsigned>(this->storage_->payload_max_size(RadarPayloadTarget::DEVICE_CONFIG)),
        wifi_connected ? "true" : "false", ssid.c_str(), rssi);
  request->send(stream);
}

}  // namespace radar_api_server
}  // namespace esphome
