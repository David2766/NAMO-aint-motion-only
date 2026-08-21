#include "system_handler.h"

#include "dashboard_assets.h"
#include "http_response.h"
#include "radar_api_server.h"
#include "setup_security.h"

#include "esphome/components/api/api_server.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"
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
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace esphome {
namespace radar_api_server {

struct SystemStatusWorkspace {
  static constexpr std::size_t CHUNK_SIZE = 768;

  struct MemoryStatus {
    uint32_t free_heap{0};
    uint32_t min_free_heap{0};
    std::size_t internal_total{0};
    std::size_t internal_free{0};
    std::size_t internal_min_free{0};
    std::size_t internal_largest_free{0};
    std::size_t httpd_stack_min_free{0};
    std::size_t psram_total{0};
    std::size_t psram_free{0};
    InternalRamWatermarkDiagnostics internal_watermark{};
  };

  char chunk[CHUNK_SIZE]{};
  wifi_ap_record_t wifi_ap{};
  esp_netif_ip_info_t ip_info{};
  RadarStorageStatus storage_status{};
  MemoryStatus memory{};
  bool wifi_connected{false};
  uint8_t mac[6]{};
  char ssid[33]{};
  char mac_text[18]{};
  char ip_text[16]{};
  char device_id[32]{};
  char device_name[32]{};
  char host[64]{};
};

namespace {

static const char *const TAG = "radar_system";
static constexpr uint32_t RESET_REBOOT_DELAY_MS = 2500;
static constexpr int64_t BOOT_GUARD_SECONDS = 60;

class StatusChunkWriter {
 public:
  StatusChunkWriter(httpd_req_t *request, char *buffer, std::size_t capacity)
      : request_(request), buffer_(buffer), capacity_(capacity) {}

  bool write(const char *value) {
    if (!this->ok_ || value == nullptr)
      return false;
    return this->append_(value, std::strlen(value));
  }

  bool boolean(bool value) { return this->write(value ? "true" : "false"); }

  bool unsigned_number(uint64_t value) {
    if (!this->ok_ || this->buffer_ == nullptr || this->capacity_ == 0)
      return false;

    uint64_t divisor = 1;
    while (value / divisor >= 10)
      divisor *= 10;
    do {
      if (!this->append_char_(static_cast<char>(
              '0' + static_cast<unsigned>(value / divisor))))
        return false;
      value %= divisor;
      divisor /= 10;
    } while (divisor != 0);
    return true;
  }

  bool signed_number(int64_t value) {
    if (value >= 0)
      return this->unsigned_number(static_cast<uint64_t>(value));
    if (!this->write("-"))
      return false;
    const uint64_t magnitude =
        static_cast<uint64_t>(-(value + 1)) + 1;
    return this->unsigned_number(magnitude);
  }

  bool json_string(const char *value) {
    if (!this->ok_ || this->buffer_ == nullptr || this->capacity_ < 8)
      return false;
    if (value == nullptr)
      value = "";
    static constexpr char HEX[] = "0123456789abcdef";
    if (!this->append_char_('"'))
      return false;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
      const unsigned char ch = static_cast<unsigned char>(*cursor);
      if (ch == '"' || ch == '\\') {
        if (!this->append_char_('\\') ||
            !this->append_char_(static_cast<char>(ch)))
          return false;
      } else if (ch < 0x20) {
        if (!this->append_char_('\\') || !this->append_char_('u') ||
            !this->append_char_('0') || !this->append_char_('0') ||
            !this->append_char_(HEX[(ch >> 4) & 0x0F]) ||
            !this->append_char_(HEX[ch & 0x0F]))
          return false;
      } else {
        if (!this->append_char_(static_cast<char>(ch)))
          return false;
      }
    }
    return this->append_char_('"');
  }

  bool finish() {
    const bool content_ok = this->flush_();
    const bool final_chunk_ok =
        httpd_resp_send_chunk(this->request_, nullptr, 0) == ESP_OK;
    this->ok_ = content_ok && final_chunk_ok;
    return this->ok_;
  }

 private:
  bool append_char_(char value) { return this->append_(&value, 1); }

  bool append_(const char *data, std::size_t size) {
    if (!this->ok_ || data == nullptr || this->buffer_ == nullptr ||
        this->capacity_ == 0)
      return false;
    while (size != 0) {
      if (this->used_ == this->capacity_ && !this->flush_())
        return false;
      const std::size_t available = this->capacity_ - this->used_;
      const std::size_t chunk_size = size < available ? size : available;
      std::memcpy(this->buffer_ + this->used_, data, chunk_size);
      this->used_ += chunk_size;
      data += chunk_size;
      size -= chunk_size;
    }
    return true;
  }

  bool flush_() {
    if (!this->ok_)
      return false;
    if (this->used_ == 0)
      return true;
    this->ok_ = httpd_resp_send_chunk(this->request_, this->buffer_,
                                      this->used_) == ESP_OK;
    if (this->ok_)
      this->used_ = 0;
    return this->ok_;
  }

  httpd_req_t *request_;
  char *buffer_;
  std::size_t capacity_;
  std::size_t used_{0};
  bool ok_{true};
};

bool append_status_field(StatusChunkWriter *writer, const char *prefix,
                         const char *value) {
  if (writer == nullptr || !writer->write(prefix))
    return false;
  return writer->json_string(value);
}

bool append_status_unsigned(StatusChunkWriter *writer, const char *prefix,
                            uint64_t value) {
  if (writer == nullptr || !writer->write(prefix))
    return false;
  return writer->unsigned_number(value);
}

bool append_status_signed(StatusChunkWriter *writer, const char *prefix,
                          int64_t value) {
  if (writer == nullptr || !writer->write(prefix))
    return false;
  return writer->signed_number(value);
}

bool append_status_bool(StatusChunkWriter *writer, const char *prefix,
                        bool value) {
  if (writer == nullptr || !writer->write(prefix))
    return false;
  return writer->boolean(value);
}

char status_hex_digit(uint8_t value, bool uppercase) {
  return value < 10
             ? static_cast<char>('0' + value)
             : static_cast<char>((uppercase ? 'A' : 'a') + value - 10);
}

void write_status_hex_byte(char *output, uint8_t value, bool uppercase) {
  output[0] = status_hex_digit(static_cast<uint8_t>(value >> 4), uppercase);
  output[1] = status_hex_digit(static_cast<uint8_t>(value & 0x0F), uppercase);
}

char *write_status_decimal_byte(char *output, uint8_t value) {
  if (value >= 100) {
    *output++ = static_cast<char>('0' + value / 100);
    value = static_cast<uint8_t>(value % 100);
    *output++ = static_cast<char>('0' + value / 10);
    *output++ = static_cast<char>('0' + value % 10);
  } else if (value >= 10) {
    *output++ = static_cast<char>('0' + value / 10);
    *output++ = static_cast<char>('0' + value % 10);
  } else {
    *output++ = static_cast<char>('0' + value);
  }
  return output;
}

void build_status_identity_text(SystemStatusWorkspace *workspace) {
  if (workspace == nullptr)
    return;
  esp_read_mac(workspace->mac, ESP_MAC_WIFI_STA);

  for (std::size_t index = 0; index < 6; ++index) {
    const std::size_t offset = index * 3;
    write_status_hex_byte(workspace->mac_text + offset,
                          workspace->mac[index], true);
    workspace->mac_text[offset + 2] = index == 5 ? '\0' : ':';
  }

  static constexpr char DEVICE_ID_PREFIX[] = "presence-sensor-";
  static constexpr char DEVICE_NAME_PREFIX[] = "Presence Sensor ";
  std::memcpy(workspace->device_id, DEVICE_ID_PREFIX,
              sizeof(DEVICE_ID_PREFIX) - 1);
  std::memcpy(workspace->device_name, DEVICE_NAME_PREFIX,
              sizeof(DEVICE_NAME_PREFIX) - 1);
  for (std::size_t index = 0; index < 3; ++index) {
    write_status_hex_byte(
        workspace->device_id + sizeof(DEVICE_ID_PREFIX) - 1 + index * 2,
        workspace->mac[index + 3], false);
    write_status_hex_byte(
        workspace->device_name + sizeof(DEVICE_NAME_PREFIX) - 1 + index * 2,
        workspace->mac[index + 3], true);
  }
  workspace->device_id[sizeof(DEVICE_ID_PREFIX) - 1 + 6] = '\0';
  workspace->device_name[sizeof(DEVICE_NAME_PREFIX) - 1 + 6] = '\0';

  static constexpr char HOST_SUFFIX[] = ".local";
  const char *app_name = App.get_name().c_str();
  const std::size_t app_name_length =
      strnlen(app_name, sizeof(workspace->host) - sizeof(HOST_SUFFIX));
  std::memcpy(workspace->host, app_name, app_name_length);
  std::memcpy(workspace->host + app_name_length, HOST_SUFFIX,
              sizeof(HOST_SUFFIX));
}

void read_status_network(SystemStatusWorkspace *workspace) {
  if (workspace == nullptr)
    return;
  workspace->ssid[0] = '\0';
  workspace->ip_text[0] = '\0';
  workspace->wifi_connected = false;
  workspace->wifi_ap = {};
  workspace->ip_info = {};

  const bool radio_connected =
      esp_wifi_sta_get_ap_info(&workspace->wifi_ap) == ESP_OK;
  workspace->wifi_connected =
      wifi::global_wifi_component != nullptr &&
      wifi::global_wifi_component->is_connected();
  if (radio_connected && workspace->wifi_connected) {
    std::strncpy(
        workspace->ssid,
        reinterpret_cast<const char *>(workspace->wifi_ap.ssid),
        sizeof(workspace->ssid) - 1);
    workspace->ssid[sizeof(workspace->ssid) - 1] = '\0';
  }

  auto *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta_netif != nullptr &&
      esp_netif_get_ip_info(sta_netif, &workspace->ip_info) == ESP_OK &&
      workspace->ip_info.ip.addr != 0) {
    char *cursor = workspace->ip_text;
    for (std::size_t index = 0; index < 4; ++index) {
      if (index != 0)
        *cursor++ = '.';
      cursor = write_status_decimal_byte(
          cursor, esp_ip4_addr_get_byte(&workspace->ip_info.ip, index));
    }
    *cursor = '\0';
  }
}

bool status_wifi_connected(const SystemStatusWorkspace &workspace) {
  return workspace.wifi_connected;
}

int status_wifi_rssi(const SystemStatusWorkspace &workspace) {
  return status_wifi_connected(workspace)
             ? static_cast<int>(workspace.wifi_ap.rssi)
             : 0;
}

uint32_t boot_guard_remaining_seconds() {
  const int64_t uptime_seconds = esp_timer_get_time() / 1000000LL;
  if (uptime_seconds >= BOOT_GUARD_SECONDS)
    return 0;
  return static_cast<uint32_t>(BOOT_GUARD_SECONDS - uptime_seconds);
}

bool reject_reboot_during_boot_guard(AsyncWebServerRequest *request) {
  const uint32_t remaining_seconds = boot_guard_remaining_seconds();
  if (remaining_seconds == 0)
    return false;

  char detail[48];
  std::snprintf(detail, sizeof(detail), "{\"remainingSeconds\":%u}",
                static_cast<unsigned>(remaining_seconds));
  http_response::send_error_info(request, 403, "boot_guard_active", "boot_guard_active", "warning", detail);
  return true;
}

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

SystemHandler::~SystemHandler() {
  if (this->status_workspace_ == nullptr)
    return;
#if defined(USE_ESP32) && defined(USE_PSRAM)
  if (this->status_workspace_in_psram_) {
    this->status_workspace_->~SystemStatusWorkspace();
    heap_caps_free(this->status_workspace_);
    this->status_workspace_ = nullptr;
    return;
  }
#endif
  delete this->status_workspace_;
  this->status_workspace_ = nullptr;
}

void SystemHandler::setup() {
  if (this->status_workspace_ == nullptr) {
#ifdef USE_PSRAM
    void *workspace_memory = heap_caps_malloc(
        sizeof(SystemStatusWorkspace),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (workspace_memory != nullptr) {
      this->status_workspace_ =
          new (workspace_memory) SystemStatusWorkspace();
      this->status_workspace_in_psram_ = true;
    }
#endif
    if (this->status_workspace_ == nullptr) {
      this->status_workspace_ =
          new (std::nothrow) SystemStatusWorkspace();
      this->status_workspace_in_psram_ = false;
    }
    if (this->status_workspace_ == nullptr) {
      ESP_LOGE(TAG, "System status workspace allocation failed size=%u",
               static_cast<unsigned>(sizeof(SystemStatusWorkspace)));
    } else if (this->status_workspace_in_psram_) {
      ESP_LOGI(TAG, "System status workspace allocated in PSRAM size=%u",
               static_cast<unsigned>(sizeof(SystemStatusWorkspace)));
    } else {
      ESP_LOGW(TAG,
               "System status workspace allocated in internal RAM size=%u",
               static_cast<unsigned>(sizeof(SystemStatusWorkspace)));
    }
  }

  if (esp_flash_get_size(
          nullptr, &this->static_resource_info_.flash_total_bytes) != ESP_OK) {
    this->static_resource_info_.flash_total_bytes = 0;
  }
  const esp_partition_t *running_partition =
      esp_ota_get_running_partition();
  const esp_partition_t *next_ota_partition =
      esp_ota_get_next_update_partition(running_partition);
  this->static_resource_info_.firmware_slot_bytes =
      running_partition != nullptr ? running_partition->size : 0;
  this->static_resource_info_.ota_slot_bytes =
      next_ota_partition != nullptr ? next_ota_partition->size : 0;
  this->static_resource_info_.firmware_used_bytes = 0;
  if (running_partition != nullptr) {
    esp_partition_pos_t running_pos{};
    running_pos.offset = running_partition->address;
    running_pos.size = running_partition->size;
    esp_image_metadata_t metadata{};
    if (esp_image_get_metadata(&running_pos, &metadata) == ESP_OK)
      this->static_resource_info_.firmware_used_bytes = metadata.image_len;
  }
}

bool SystemHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET && url == "/api/system/status")
    return true;
  if (request->method() == HTTP_GET && url == "/api/system/api-key")
    return true;
  if (request->method() == HTTP_POST && url == "/api/system/api-key/rotate")
    return true;
  if (request->method() == HTTP_POST && url == "/api/system/ha-setup-handoff")
    return true;
  if (request->method() == HTTP_POST && url == "/api/system/reboot")
    return true;
  return request->method() == HTTP_POST && url == "/api/system/reset";
}

bool SystemHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (request->method() == HTTP_POST && url == "/api/system/reset") {
    this->handle_reset_(request);
  } else if (request->method() == HTTP_POST && url == "/api/system/reboot") {
    this->handle_reboot_(request);
  } else if (request->method() == HTTP_POST && url == "/api/system/ha-setup-handoff") {
    this->handle_ha_setup_handoff_(request);
  } else if (request->method() == HTTP_GET && url == "/api/system/api-key") {
    this->handle_api_key_(request);
  } else if (request->method() == HTTP_POST &&
             url == "/api/system/api-key/rotate") {
    this->handle_api_key_rotate_(request);
  } else {
    this->handle_status_(request);
  }
  return true;
}

void SystemHandler::handle_api_key_rotate_(
    AsyncWebServerRequest *request) {
#ifdef USE_API_NOISE
  if (api::global_api_server == nullptr) {
    http_response::send_error_info(request, 503, "api_key_unavailable",
                                   "api_key_unavailable", "error", "{}");
    return;
  }

  api::psk_t api_key{};
  if (!generate_api_key(&api_key) || psk_equals(api_key, DEMO_API_PSK)) {
    http_response::send_error_info(request, 500, "api_key_random_failed",
                                   "api_key_random_failed", "error", "{}");
    return;
  }
  if (!api::global_api_server->save_noise_psk(api_key, true)) {
    http_response::send_error_info(request, 500, "api_key_save_failed",
                                   "api_key_save_failed", "error", "{}");
    return;
  }

  const auto encoded =
      json_escape(encode_base64_psk(api_key).c_str());
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"apiKey\":\"%s\",\"visibleSeconds\":30,"
      "\"statusInfo\":{\"code\":\"api_key_rotated\","
      "\"severity\":\"info\",\"detail\":{}}}",
      encoded.c_str());
  request->send(stream);
#else
  http_response::send_error_info(request, 503, "api_key_unsupported",
                                 "api_key_unsupported", "error", "{}");
#endif
}

void SystemHandler::handle_reboot_(AsyncWebServerRequest *request) {
  if (reject_reboot_during_boot_guard(request))
    return;

  auto *stream = request->beginResponseStream("application/json");
  stream->printf("{\"ok\":true,\"rebootInMs\":%u}", static_cast<unsigned>(RESET_REBOOT_DELAY_MS));
  request->send(stream);
  this->server_->reboot_after(RESET_REBOOT_DELAY_MS);
}

void SystemHandler::handle_api_key_(AsyncWebServerRequest *request) {
#ifdef USE_API_NOISE
  if (api::global_api_server == nullptr) {
    http_response::send_error_info(request, 503, "api_key_unavailable", "api_key_unavailable", "error", "{}");
    return;
  }
  const auto &ctx = api::global_api_server->get_noise_ctx();
  if (!ctx.has_psk()) {
    http_response::send_error_info(request, 404, "api_key_not_set", "api_key_not_set", "error", "{}");
    return;
  }
  const auto api_key = json_escape(encode_base64_psk(ctx.get_psk()).c_str());
  auto *stream = request->beginResponseStream("application/json");
  stream->printf("{\"ok\":true,\"apiKey\":\"%s\",\"visibleSeconds\":30}", api_key.c_str());
  request->send(stream);
#else
  http_response::send_error_info(request, 503, "api_key_unsupported", "api_key_unsupported", "error", "{}");
#endif
}

void SystemHandler::handle_ha_setup_handoff_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (wifi::global_wifi_component == nullptr) {
    http_response::send_error_info(request, 503, "wifi_unavailable", "wifi_unavailable", "error", "{}");
    return;
  }
  wifi_config_t config{};
  const auto config_err = esp_wifi_get_config(WIFI_IF_STA, &config);
  const auto ssid_len = strnlen(reinterpret_cast<const char *>(config.sta.ssid), sizeof(config.sta.ssid));
  if (config_err != ESP_OK || ssid_len == 0) {
    ESP_LOGE(TAG, "HA setup handoff requested without STA config config=%d ssid_len=%u", static_cast<int>(config_err),
             static_cast<unsigned>(ssid_len));
    http_response::send_error_info(request, 409, "wifi_not_ready", "wifi_not_ready", "warning", "{}");
    return;
  }
#ifdef USE_API_NOISE
  if (api::global_api_server == nullptr ||
      !api::global_api_server->get_noise_ctx().has_psk()) {
    http_response::send_error_info(request, 409, "api_key_not_set",
                                   "api_key_not_set", "warning", "{}");
    return;
  }
#endif
  // User-confirmed handoff: ESP-IDF has validated Wi-Fi, now ESPHome resumes native API ownership.
  ESP_LOGI(TAG, "HA setup handoff accepted, rebooting into ESPHome WiFi ownership");
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"ha_setup_handoff_started\",\"waitSeconds\":30,"
                "\"rebootExpected\":true,"
                "\"statusInfo\":{\"code\":\"ha_handoff_started\",\"severity\":\"info\","
                "\"detail\":{\"waitSeconds\":30,\"rebootExpected\":true}}}");
  this->server_->complete_setup_handoff_after(750);
#else
  http_response::send_error_info(request, 503, "wifi_unsupported", "wifi_unsupported", "error", "{}");
#endif
}

bool SystemHandler::request_bool_(AsyncWebServerRequest *request, const char *key, bool default_value) const {
  if (request->hasArg(key)) {
    const auto value = request->arg(key);
    return value == "1" || value == "true" || value == "on";
  }

  if (!request->hasArg("data"))
    return default_value;
  const auto body = request->arg("data");
  const std::string true_pattern = std::string("\"") + key + "\":true";
  const std::string false_pattern = std::string("\"") + key + "\":false";
  if (body.find(true_pattern) != std::string::npos)
    return true;
  if (body.find(false_pattern) != std::string::npos)
    return false;
  return default_value;
}

bool SystemHandler::reset_device_config_(bool reset_space,
                                         bool reset_detection) const {
  std::string current;
  const bool has_config = this->storage_->read_payload(
      RadarPayloadTarget::DEVICE_CONFIG, &current);

  JsonDocument source;
  if (has_config && deserializeJson(source, current) !=
                        DeserializationError::Ok) {
    ESP_LOGE(TAG, "Stored device config is not valid JSON");
    return false;
  }

  JsonDocument updated;
  if (reset_detection) {
    updated["version"] = 1;
    updated["zones"].to<JsonArray>();
    updated["calibrationZones"].to<JsonArray>();
    if (!reset_space && has_config) {
      const JsonVariantConst floorplan = source["floorplan"];
      if (!floorplan.isNull())
        updated["floorplan"].set(floorplan);
    }
  } else if (reset_space) {
    if (!has_config) {
      this->device_config_cache_->clear();
      return true;
    }
    updated.set(source);
    updated.as<JsonObject>().remove("floorplan");
  } else {
    return true;
  }

  std::string encoded;
  serializeJson(updated, encoded);
  if (encoded.empty() ||
      !this->storage_->write_payload(
          RadarPayloadTarget::DEVICE_CONFIG,
          reinterpret_cast<const uint8_t *>(encoded.data()),
          static_cast<uint32_t>(encoded.size())))
    return false;

  this->device_config_cache_->update(std::move(encoded));
  return true;
}

bool SystemHandler::reset_stats_() const {
  if (!this->storage_->delete_payload(RadarPayloadTarget::STATS))
    return false;
  this->stats_store_->clear();
  return true;
}

void SystemHandler::handle_reset_(AsyncWebServerRequest *request) {
  const bool legacy_settings =
      this->request_bool_(request, "settings", false);
  const bool reset_space =
      this->request_bool_(request, "space", legacy_settings);
  const bool reset_detection =
      this->request_bool_(request, "detection", legacy_settings);
  const bool reset_connections =
      this->request_bool_(request, "connections", false);
  const bool reset_wifi = this->request_bool_(request, "wifi", false);
  const bool reset_stats = this->request_bool_(request, "stats", false);
  const bool reboot_required =
      reset_space || reset_detection || reset_connections || reset_wifi;

  if (!reset_space && !reset_detection && !reset_connections &&
      !reset_wifi && !reset_stats) {
    http_response::send_error_info(request, 400, "nothing_selected", "nothing_selected", "warning",
                                   R"({"target":"system_reset"})");
    return;
  }

  if (reboot_required && reject_reboot_during_boot_guard(request))
    return;

  if (reset_space &&
      !this->storage_->delete_floorplan()) {
    http_response::send_error_info(request, 500, "space_reset_failed",
                                   "space_reset_failed", "error",
                                   R"({"target":"space"})");
    return;
  }

  if ((reset_space || reset_detection) &&
      !this->reset_device_config_(reset_space, reset_detection)) {
    http_response::send_error_info(
        request, 500,
        reset_detection ? "detection_reset_failed" : "space_reset_failed",
        reset_detection ? "detection_reset_failed" : "space_reset_failed",
        "error",
        reset_detection ? R"({"target":"detection"})"
                        : R"({"target":"space"})");
    return;
  }

  if (reset_detection)
    this->server_->request_general_settings_reset();

  if (reset_connections) {
    if (!this->server_->reset_platform_relationships()) {
      http_response::send_error_info(
          request, 500, "connections_reset_failed",
          "connections_reset_failed", "error",
          R"({"target":"connections"})");
      return;
    }
  }

  if (reset_stats) {
    if (!this->reset_stats_()) {
      http_response::send_error_info(request, 500, "stats_reset_failed", "stats_reset_failed", "error",
                                     R"({"target":"stats"})");
      return;
    }
  }

  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"reset\":{\"space\":%s,\"detection\":%s,"
      "\"connections\":%s,\"wifi\":%s,\"stats\":%s},"
      "\"details\":{\"floorplan\":%s,\"deviceConfig\":%s,"
      "\"connections\":%s,\"stats\":%s,\"wifiScheduled\":%s},"
      "\"rebootRequired\":%s,\"rebootInMs\":%u}",
      reset_space ? "true" : "false",
      reset_detection ? "true" : "false",
      reset_connections ? "true" : "false",
      reset_wifi ? "true" : "false",
      reset_stats ? "true" : "false",
      reset_space ? "true" : "false",
      (reset_space || reset_detection) ? "true" : "false",
      reset_connections ? "true" : "false",
      reset_stats ? "true" : "false",
      reset_wifi ? "true" : "false",
      reboot_required ? "true" : "false",
      static_cast<unsigned>(RESET_REBOOT_DELAY_MS));
  request->send(stream);

  if (reset_wifi) {
    this->server_->reset_wifi_credentials_and_reboot_after(RESET_REBOOT_DELAY_MS);
  } else if (reboot_required) {
    this->server_->reboot_after(RESET_REBOOT_DELAY_MS);
  }
}

void SystemHandler::handle_status_(AsyncWebServerRequest *request) {
  if (this->status_workspace_ == nullptr) {
    http_response::send_error_info(
        request, 503, "system_status_unavailable",
        "system_status_unavailable", "error",
        R"({"resource":"status_workspace"})");
    return;
  }

  SystemStatusWorkspace *workspace = this->status_workspace_;
  build_status_identity_text(workspace);
  read_status_network(workspace);
  workspace->storage_status = this->storage_->status();

  httpd_req_t *raw_request = *request;
  httpd_resp_set_status(raw_request, HTTPD_200);
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Accept-Ranges", "none");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  StatusChunkWriter writer(raw_request, workspace->chunk,
                           sizeof(workspace->chunk));
  writer.write("{\"ok\":true");

  writer.write(",\"device\":{\"type\":\"presence-sensor\",\"id\":");
  writer.json_string(workspace->device_id);
  append_status_field(&writer, ",\"name\":", workspace->device_name);
  writer.write(",\"dashboardPath\":\"/dashboard\"}");

  {
    const auto &identity = this->server_->platform_identity();
    const bool identity_ready = this->server_->platform_identity_ready();
    const auto &registry =
        this->server_->participant_registry_snapshot().registry;
    const bool registry_ready =
        this->server_->participant_registry_persistence_ready();
    const bool readiness_ready =
        identity_ready &&
        this->server_->participant_readiness_persistence_ready();
    const char *mode =
        identity_ready &&
                this->server_->platform_site_membership().mode ==
                    SiteMembershipMode::ENROLLED
            ? "enrolled"
            : "standalone";
    writer.write(
        ",\"platform\":{\"schemaVersion\":1,\"identityReady\":");
    writer.boolean(identity_ready);
    if (identity_ready) {
      append_status_field(&writer, ",\"siteId\":", identity.site_id);
      append_status_field(&writer, ",\"nodeId\":", identity.node_id);
    }
    append_status_field(&writer, ",\"mode\":", mode);
    append_status_unsigned(
        &writer, ",\"maxParticipants\":",
        ParticipantRegistry::MAX_PARTICIPANTS);
    append_status_bool(&writer, ",\"registryReady\":", registry_ready);
    append_status_unsigned(
        &writer, ",\"remoteParticipantCount\":",
        registry_ready ? registry.size() : 0);
    append_status_unsigned(
        &writer, ",\"connectedRemoteParticipantCount\":",
        registry_ready ? registry.connected_count() : 0);
    append_status_unsigned(
        &writer, ",\"activeRemoteParticipantCount\":",
        registry_ready ? registry.active_count() : 0);
    append_status_bool(&writer, ",\"readinessReady\":", readiness_ready);
    append_status_unsigned(
        &writer, ",\"configuredRemoteParticipantCount\":",
        readiness_ready
            ? this->server_->participant_readiness_snapshot().registry.size()
            : 0);
    append_status_bool(&writer, ",\"fusionEnabled\":",
                       this->server_->production_fusion_build_enabled());
    writer.write(",\"pairingSupported\":true");
    append_status_bool(
        &writer, ",\"pairingEnabled\":",
        registry_ready && identity_ready &&
            this->server_->participant_enrollment().window_open(millis()));
    writer.write("}");
  }

  writer.write(",\"network\":{\"ip\":");
  writer.json_string(workspace->ip_text);
  append_status_field(&writer, ",\"mac\":", workspace->mac_text);
  append_status_field(&writer, ",\"host\":", workspace->host);
  writer.write("}");

  writer.write(",\"firmware\":{\"version\":");
  writer.json_string(dashboard_assets::FIRMWARE_VERSION);
  writer.write(",\"buildTime\":\"" __DATE__ " " __TIME__ "\"");
  append_status_signed(&writer, ",\"uptimeSeconds\":",
                       esp_timer_get_time() / 1000000LL);
  writer.write("}");

  {
#ifdef USE_API
    const bool api_enabled = api::global_api_server != nullptr;
#ifdef USE_API_NOISE
    const bool api_key_configured =
        api_enabled && api::global_api_server->get_noise_ctx().has_psk();
#else
    const bool api_key_configured = api_enabled;
#endif
    const bool api_service_ready =
        status_wifi_connected(*workspace) && api_enabled && api_key_configured;
    const bool api_connected =
        api_enabled && api::global_api_server->is_connected();
    const bool api_warning =
        api_enabled &&
        api::global_api_server->status_has_warning();
#else
    const bool api_enabled = false;
    const bool api_key_configured = false;
    const bool api_service_ready = false;
    const bool api_connected = false;
    const bool api_warning = false;
#endif
    const char *api_status_code =
        !api_service_ready
            ? (api_key_configured ? "api_service_not_ready"
                                  : "api_key_not_set")
            : (api_connected ? "api_client_connected"
                             : "api_client_waiting");
    writer.write(",\"api\":{\"enabled\":");
    writer.boolean(api_enabled);
    append_status_bool(&writer, ",\"keyConfigured\":",
                       api_key_configured);
    append_status_bool(&writer, ",\"serviceReady\":", api_service_ready);
    append_status_bool(&writer, ",\"clientConnected\":", api_connected);
    append_status_bool(&writer, ",\"connected\":", api_connected);
    append_status_bool(&writer, ",\"warning\":", api_warning);
    writer.write(",\"statusInfo\":{\"code\":");
    writer.json_string(api_status_code);
    append_status_field(&writer, ",\"severity\":", "info");
    writer.write(",\"detail\":{}}}");
  }

  const uint32_t boot_guard_remaining = boot_guard_remaining_seconds();
  writer.write(",\"boot\":{\"initialGuardActive\":");
  writer.boolean(boot_guard_remaining > 0);
  append_status_signed(&writer, ",\"guardSeconds\":", BOOT_GUARD_SECONDS);
  append_status_unsigned(&writer, ",\"remainingSeconds\":",
                         boot_guard_remaining);
  writer.write("},\"dashboard\":{\"version\":");
  writer.json_string(dashboard_assets::DASHBOARD_VERSION);
  append_status_unsigned(&writer, ",\"gzipBytes\":",
                         dashboard_assets::DASHBOARD_TOTAL_GZ_SIZE);
  writer.write("},\"schema\":{\"config\":");
  writer.signed_number(dashboard_assets::CONFIG_SCHEMA_VERSION);
  append_status_signed(&writer, ",\"floorplan\":",
                       dashboard_assets::FLOORPLAN_SCHEMA_VERSION);
  append_status_signed(&writer, ",\"stats\":",
                       dashboard_assets::STATS_SCHEMA_VERSION);
  writer.write("}");

  const auto state_publisher =
      this->server_->state_publisher_diagnostics();
  writer.write(",\"statePublisher\":{\"attempted\":");
  writer.unsigned_number(state_publisher.attempted);
  append_status_unsigned(&writer, ",\"published\":",
                         state_publisher.published);
  append_status_unsigned(&writer, ",\"skippedBusyBuffer\":",
                         state_publisher.skipped_busy_buffer);
  append_status_unsigned(&writer, ",\"failedCommit\":",
                         state_publisher.failed_commit);
  writer.write("}");

  workspace->memory.free_heap = esp_get_free_heap_size();
  workspace->memory.min_free_heap = esp_get_minimum_free_heap_size();
  workspace->memory.internal_total =
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  workspace->memory.internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  workspace->memory.internal_min_free =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  workspace->memory.internal_largest_free =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                       MALLOC_CAP_8BIT);
  this->server_->record_httpd_stack_watermark("/api/system/status");
  this->server_->copy_internal_ram_watermark(
      &workspace->memory.internal_watermark);
  workspace->memory.httpd_stack_min_free =
      static_cast<std::size_t>(uxTaskGetStackHighWaterMark(nullptr)) *
      sizeof(StackType_t);
  workspace->memory.psram_total =
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  workspace->memory.psram_free =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  writer.write(",\"memory\":{\"freeHeap\":");
  writer.unsigned_number(workspace->memory.free_heap);
  append_status_unsigned(&writer, ",\"minFreeHeap\":",
                         workspace->memory.min_free_heap);
  append_status_unsigned(&writer, ",\"internalTotalBytes\":",
                         workspace->memory.internal_total);
  append_status_unsigned(&writer, ",\"internalFreeBytes\":",
                         workspace->memory.internal_free);
  append_status_unsigned(&writer, ",\"internalMinFreeBytes\":",
                         workspace->memory.internal_min_free);
  append_status_unsigned(&writer, ",\"internalLargestFreeBlockBytes\":",
                         workspace->memory.internal_largest_free);
  append_status_unsigned(&writer, ",\"httpdStackMinFreeBytes\":",
                         workspace->memory.httpd_stack_min_free);
  append_status_field(&writer, ",\"httpdStackLowestEndpoint\":",
                      this->server_->httpd_stack_lowest_endpoint());
  append_status_field(&writer, ",\"httpdStackLowestPhase\":",
                      this->server_->httpd_stack_lowest_phase());
  append_status_unsigned(
      &writer, ",\"httpdStackLowestEndpointFreeBytes\":",
      this->server_->httpd_stack_lowest_endpoint_free_bytes());
  append_status_field(&writer, ",\"internalMinFreeSource\":",
                      workspace->memory.internal_watermark.source);
  append_status_field(&writer, ",\"internalMinFreePhase\":",
                      workspace->memory.internal_watermark.phase);
  append_status_unsigned(
      &writer, ",\"internalMinFreeObservedAtMs\":",
      workspace->memory.internal_watermark.observed_at_ms);
  append_status_unsigned(
      &writer, ",\"internalMinFreeEventCount\":",
      workspace->memory.internal_watermark.event_count);
  append_status_unsigned(
      &writer, ",\"internalMinLargestFreeBlockBytes\":",
      workspace->memory.internal_watermark.largest_free_block_bytes);
  if (workspace->memory.internal_watermark.threshold_recorded) {
    append_status_field(
        &writer, ",\"internalThresholdSource\":",
        workspace->memory.internal_watermark.threshold_source);
    append_status_field(
        &writer, ",\"internalThresholdPhase\":",
        workspace->memory.internal_watermark.threshold_phase);
    append_status_unsigned(
        &writer, ",\"internalThresholdFreeBytes\":",
        workspace->memory.internal_watermark.threshold_free_bytes);
    append_status_unsigned(
        &writer, ",\"internalThresholdObservedAtMs\":",
        workspace->memory.internal_watermark.threshold_observed_at_ms);
  }
  append_status_unsigned(&writer, ",\"psramTotal\":",
                         workspace->memory.psram_total);
  append_status_unsigned(&writer, ",\"psramFree\":",
                         workspace->memory.psram_free);
  append_status_unsigned(&writer, ",\"externalTotalBytes\":",
                         workspace->memory.psram_total);
  append_status_unsigned(&writer, ",\"externalFreeBytes\":",
                         workspace->memory.psram_free);
  writer.write("}");

  const RadarStorageStatus &storage_status = workspace->storage_status;
  writer.write(",\"flash\":{\"totalBytes\":");
  writer.unsigned_number(this->static_resource_info_.flash_total_bytes);
  append_status_unsigned(&writer, ",\"firmwareUsedBytes\":",
                         this->static_resource_info_.firmware_used_bytes);
  append_status_unsigned(&writer, ",\"firmwareSlotBytes\":",
                         this->static_resource_info_.firmware_slot_bytes);
  append_status_unsigned(&writer, ",\"otaSlotBytes\":",
                         this->static_resource_info_.ota_slot_bytes);
  append_status_unsigned(&writer, ",\"storageUsedBytes\":",
                         storage_status.used_bytes);
  append_status_unsigned(&writer, ",\"storageTotalBytes\":",
                         storage_status.total_bytes);
  writer.write("},\"storage\":{\"ok\":");
  writer.boolean(storage_status.ok);
  append_status_field(&writer, ",\"partition\":",
                      storage_status.partition_label);
  append_status_unsigned(&writer, ",\"totalBytes\":",
                         storage_status.total_bytes);
  append_status_unsigned(&writer, ",\"usedBytes\":",
                         storage_status.used_bytes);
  append_status_unsigned(&writer, ",\"floorplanConfigBytes\":",
                         storage_status.header.config_size);
  append_status_unsigned(&writer, ",\"floorplanImageBytes\":",
                         storage_status.header.image_size);
  append_status_unsigned(&writer, ",\"deviceConfigBytes\":",
                         storage_status.header.reserved[0]);
  append_status_unsigned(&writer, ",\"statsBytes\":",
                         storage_status.header.reserved[1]);
  append_status_unsigned(
      &writer, ",\"maxPayloadBytes\":",
      this->storage_->payload_max_size(RadarPayloadTarget::DEVICE_CONFIG));
  writer.write("}");

  writer.write(",\"wifi\":{\"connected\":");
  writer.boolean(status_wifi_connected(*workspace));
  append_status_field(&writer, ",\"ssid\":", workspace->ssid);
  append_status_signed(&writer, ",\"rssi\":",
                       status_wifi_rssi(*workspace));
  writer.write("}");
  writer.write(
      ",\"bluetooth\":{\"enabled\":true,\"connected\":false}}");
  if (!writer.finish())
    ESP_LOGW(TAG, "System status chunked response failed");
}

}  // namespace radar_api_server
}  // namespace esphome
