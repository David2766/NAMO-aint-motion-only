#include "setup_wifi_manager.h"

#include "radar_api_server.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#ifdef USE_ESP32
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#endif

namespace esphome {
namespace radar_api_server {

namespace {
static const char *const TAG = "radar_setup";
}

#ifndef RADAR_SETUP_WIFI_VERBOSE
#define RADAR_SETUP_WIFI_VERBOSE 0
#endif

SetupWifiStage setup_wifi_stage = SetupWifiStage::IDLE;
std::string setup_wifi_ssid;
std::string setup_wifi_psk;
std::string setup_wifi_connected_ip;
uint32_t setup_wifi_started_ms = 0;
uint32_t setup_wifi_connected_ms = 0;
uint8_t setup_wifi_disconnect_count = 0;
bool setup_wifi_credentials_saved = false;
static bool setup_wifi_events_registered = false;
bool setup_network_scan_running = false;
std::string setup_network_scan_reason;
std::vector<NetworkInfo> setup_network_cache;
uint32_t setup_network_cache_ms = 0;
bool setup_network_cache_ready = false;

// Setup provisioning is ESP-IDF led: AP, scan, temporary STA connect, and flash STA config.
// ESPHome keeps the normal runtime Wi-Fi preference for native API / Home Assistant handoff.
void reset_setup_wifi_state_() {
  setup_wifi_stage = SetupWifiStage::IDLE;
  setup_wifi_ssid.clear();
  setup_wifi_psk.clear();
  setup_wifi_connected_ip.clear();
  setup_wifi_started_ms = 0;
  setup_wifi_connected_ms = 0;
  setup_wifi_disconnect_count = 0;
  setup_wifi_credentials_saved = false;
}

const char *setup_wifi_stage_to_string(SetupWifiStage stage) {
  switch (stage) {
    case SetupWifiStage::READY:
      return "ready";
    case SetupWifiStage::CONNECTING:
      return "connecting";
    case SetupWifiStage::CONNECTED:
      return "connected";
    case SetupWifiStage::FAILED:
      return "failed";
    case SetupWifiStage::IDLE:
    default:
      return "idle";
  }
}

#ifdef USE_ESP32
static bool setup_wifi_active() {
  return setup_wifi_stage == SetupWifiStage::READY || setup_wifi_stage == SetupWifiStage::CONNECTING ||
         setup_wifi_stage == SetupWifiStage::CONNECTED || setup_wifi_stage == SetupWifiStage::FAILED;
}

static const char *setup_wifi_mode_to_string(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL:
      return "NULL";
    case WIFI_MODE_STA:
      return "STA";
    case WIFI_MODE_AP:
      return "AP";
    case WIFI_MODE_APSTA:
      return "APSTA";
    default:
      return "UNKNOWN";
  }
}

static void log_setup_wifi_mode_(const char *reason) {
#if RADAR_SETUP_WIFI_VERBOSE
  wifi_mode_t mode = WIFI_MODE_NULL;
  const auto mode_err = esp_wifi_get_mode(&mode);
  wifi_config_t ap_config{};
  const auto ap_config_err = esp_wifi_get_config(WIFI_IF_AP, &ap_config);
  ESP_LOGW("radar_setup",
           "setup_dbg: %s mode=%s(%d) mode_err=%d ap_config_err=%d ap_ssid='%s' ap_channel=%u ap_hidden=%u",
           reason, setup_wifi_mode_to_string(mode), static_cast<int>(mode), static_cast<int>(mode_err),
           static_cast<int>(ap_config_err), reinterpret_cast<const char *>(ap_config.ap.ssid),
           static_cast<unsigned>(ap_config.ap.channel), static_cast<unsigned>(ap_config.ap.ssid_hidden));
#else
  (void) reason;
#endif
}

static bool save_esphome_wifi_sta_preference_only_(const std::string &ssid, const std::string &psk) {
  if (global_preferences == nullptr) {
    ESP_LOGE("radar_setup", "setup: ESPHome WiFi preference save failed, preferences unavailable");
    return false;
  }

  wifi::SavedWifiSettings save{};
  std::strncpy(save.ssid, ssid.c_str(), sizeof(save.ssid) - 1);
  std::strncpy(save.password, psk.c_str(), sizeof(save.password) - 1);

  const uint32_t hash = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->has_sta()
                            ? App.get_config_version_hash()
                            : 88491487UL;
  auto pref = global_preferences->make_preference<wifi::SavedWifiSettings>(hash, true);
  const bool saved = pref.save(&save);
  const bool synced = global_preferences->sync();
  ESP_LOGI("radar_setup", "setup: ESPHome WiFi preference save-only hash=%u saved=%d synced=%d", hash,
           saved ? 1 : 0, synced ? 1 : 0);
  return saved && synced;
}

void persist_setup_wifi_credentials_() {
  if (setup_wifi_credentials_saved || setup_wifi_ssid.empty())
    return;

  // Persist both sides after the user confirms setup:
  // ESP-IDF flash config boots the radio, ESPHome preference feeds native API/HA runtime.
  ESP_LOGI("radar_setup", "setup: persist verified WiFi ssid='%s'", setup_wifi_ssid.c_str());
  const bool esphome_pref_saved = save_esphome_wifi_sta_preference_only_(setup_wifi_ssid, setup_wifi_psk);

  wifi_config_t config{};
  if (setup_wifi_ssid.size() > sizeof(config.sta.ssid) || setup_wifi_psk.size() > sizeof(config.sta.password)) {
    ESP_LOGE("radar_setup", "setup: skip ESP-IDF WiFi persist, invalid length ssid_len=%u password_len=%u",
             static_cast<unsigned>(setup_wifi_ssid.size()), static_cast<unsigned>(setup_wifi_psk.size()));
    return;
  }

  std::memcpy(config.sta.ssid, setup_wifi_ssid.data(), setup_wifi_ssid.size());
  if (!setup_wifi_psk.empty())
    std::memcpy(config.sta.password, setup_wifi_psk.data(), setup_wifi_psk.size());
  config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  config.sta.threshold.authmode = WIFI_AUTH_OPEN;

  const auto storage_err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  const auto config_err = esp_wifi_set_config(WIFI_IF_STA, &config);
  setup_wifi_credentials_saved = esphome_pref_saved && storage_err == ESP_OK && config_err == ESP_OK;
  if (setup_wifi_credentials_saved) {
    ESP_LOGI("radar_setup", "setup: persist verified WiFi saved");
  } else {
    ESP_LOGW("radar_setup", "setup: persist verified WiFi failed pref=%d storage=%d config=%d",
             esphome_pref_saved ? 1 : 0, static_cast<int>(storage_err), static_cast<int>(config_err));
  }
}

static void setup_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
#if RADAR_SETUP_WIFI_VERBOSE
  if (event_base == WIFI_EVENT) {
    if (event_id == WIFI_EVENT_AP_START) {
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_AP_START stage=%s active=%d",
               setup_wifi_stage_to_string(setup_wifi_stage), setup_wifi_active() ? 1 : 0);
      log_setup_wifi_mode_("after AP_START");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_AP_STOP raw stage=%s active=%d",
               setup_wifi_stage_to_string(setup_wifi_stage), setup_wifi_active() ? 1 : 0);
      log_setup_wifi_mode_("after AP_STOP raw");
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
      const auto *event = static_cast<wifi_event_ap_staconnected_t *>(event_data);
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_AP_STACONNECTED stage=%s aid=%d",
               setup_wifi_stage_to_string(setup_wifi_stage), event != nullptr ? event->aid : -1);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
      const auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(event_data);
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_AP_STADISCONNECTED stage=%s aid=%d",
               setup_wifi_stage_to_string(setup_wifi_stage), event != nullptr ? event->aid : -1);
    } else if (event_id == WIFI_EVENT_STA_START) {
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_STA_START stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
    } else if (event_id == WIFI_EVENT_STA_STOP) {
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_STA_STOP stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
      ESP_LOGW("radar_setup", "setup_dbg: WIFI_EVENT_SCAN_DONE stage=%s scan_running=%d reason=%s",
               setup_wifi_stage_to_string(setup_wifi_stage), setup_network_scan_running ? 1 : 0,
               setup_network_scan_reason.c_str());
    }
  }
#endif

  if (!setup_wifi_active()) {
    ESP_LOGD("radar_setup", "setup: ignored wifi event base=%s id=%d stage=%s", event_base, static_cast<int>(event_id),
             setup_wifi_stage_to_string(setup_wifi_stage));
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
    ESP_LOGW("radar_setup", "setup: WIFI_EVENT_AP_STOP stage=%s -> restore APSTA",
             setup_wifi_stage_to_string(setup_wifi_stage));
    const auto err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
      ESP_LOGW("radar_setup", "setup: AP_STOP restore APSTA failed result=%d", static_cast<int>(err));
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    const int reason = event != nullptr ? event->reason : -1;
    if (setup_wifi_stage == SetupWifiStage::CONNECTING && setup_wifi_disconnect_count < 255)
      setup_wifi_disconnect_count++;
    ESP_LOGI("radar_setup", "setup: WIFI_EVENT_STA_DISCONNECTED stage=%s reason=%d count=%u",
             setup_wifi_stage_to_string(setup_wifi_stage), reason, setup_wifi_disconnect_count);
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    char ip_buf[16] = "";
    if (event != nullptr) {
      snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&event->ip_info.ip));
      setup_wifi_connected_ip = ip_buf;
    }
    setup_wifi_stage = SetupWifiStage::CONNECTED;
    setup_wifi_connected_ms = millis();
    setup_wifi_disconnect_count = 0;
    ESP_LOGI("radar_setup", "setup: IP_EVENT_STA_GOT_IP stage=connected ip=%s",
             setup_wifi_connected_ip.c_str());
  }
}

void ensure_setup_wifi_events_registered() {
  if (setup_wifi_events_registered)
    return;
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &setup_wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &setup_wifi_event_handler, nullptr);
  setup_wifi_events_registered = true;
  ESP_LOGD("radar_setup", "setup: wifi event handlers registered");
}
#else
void ensure_setup_wifi_events_registered() {}
void persist_setup_wifi_credentials_() {}
#endif

bool wifi_connected() {
  return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
}

bool setup_mode_active() {
  const bool stage_active = setup_wifi_stage == SetupWifiStage::READY || setup_wifi_stage == SetupWifiStage::CONNECTING ||
                            setup_wifi_stage == SetupWifiStage::CONNECTED || setup_wifi_stage == SetupWifiStage::FAILED;
  const bool ap_active = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active();
  return (stage_active || ap_active) && !wifi_connected();
}

std::string wifi_sta_ip_address() {
#ifdef USE_ESP32
  auto *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info{};
  if (sta_netif != nullptr && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    char ip_buf[16];
    std::snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
    return ip_buf;
  }
#endif
  return "";
}

static void replace_or_update_network_(std::vector<NetworkInfo> *networks, const NetworkInfo &network) {
  auto found = std::find_if(networks->begin(), networks->end(), [&network](const NetworkInfo &item) {
    return item.ssid == network.ssid;
  });
  if (found == networks->end()) {
    networks->push_back(network);
  } else if (network.rssi > found->rssi) {
    *found = network;
  }
}

static void sort_networks_(std::vector<NetworkInfo> *networks) {
  std::sort(networks->begin(), networks->end(), [](const NetworkInfo &a, const NetworkInfo &b) {
    return a.rssi > b.rssi;
  });
}

bool cached_network_is_open_(const std::string &ssid) {
  const auto found = std::find_if(setup_network_cache.begin(), setup_network_cache.end(), [&ssid](const NetworkInfo &item) {
    return item.ssid == ssid;
  });
  return found != setup_network_cache.end() && !found->locked;
}

bool start_setup_network_scan_from_idf_(const char *reason, bool disconnect_first) {
#ifndef USE_ESP32
  return false;
#else
  if (setup_network_scan_running) {
    ESP_LOGI(TAG, "setup: WiFi scan already running reason=%s", reason);
    return false;
  }
  if (setup_wifi_stage == SetupWifiStage::CONNECTING) {
    ESP_LOGI(TAG, "setup: WiFi scan skipped during connect reason=%s", reason);
    return false;
  }

  setup_network_scan_running = true;
  setup_network_cache_ready = false;
  setup_network_scan_reason = reason != nullptr ? reason : "";

  wifi_mode_t previous_mode = WIFI_MODE_NULL;
  const auto get_mode_err = esp_wifi_get_mode(&previous_mode);
  log_setup_wifi_mode_("before scan start");
  if (disconnect_first) {
    const auto disconnect_err = esp_wifi_disconnect();
    const auto mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (get_mode_err != ESP_OK || mode_err != ESP_OK) {
      ESP_LOGW(TAG, "setup: pre-scan STA mode issue reason=%s get_mode=%d previous=%d disconnect=%d mode=%d", reason,
               static_cast<int>(get_mode_err), static_cast<int>(previous_mode), static_cast<int>(disconnect_err),
               static_cast<int>(mode_err));
    } else {
      ESP_LOGD(TAG, "setup: pre-scan STA mode reason=%s previous=%d disconnect=%d", reason,
               static_cast<int>(previous_mode), static_cast<int>(disconnect_err));
    }
  }

  wifi_scan_config_t config{};
  config.show_hidden = false;
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  config.scan_time.active.min = 80;
  config.scan_time.active.max = 260;

  const auto scan_err = esp_wifi_scan_start(&config, false);
  if (scan_err != ESP_OK) {
    setup_network_scan_running = false;
    ESP_LOGW(TAG, "setup: esp_wifi_scan_start failed reason=%s err=%d", reason, static_cast<int>(scan_err));
    log_setup_wifi_mode_("after scan start failed");
    return false;
  }
  ESP_LOGI(TAG, "setup: started WiFi scan reason=%s", reason);
  log_setup_wifi_mode_("after scan start");
  return true;
#endif
}

bool finish_setup_network_scan_from_idf_(const char *reason) {
#ifndef USE_ESP32
  return false;
#else
  uint16_t ap_count = 0;
  const auto count_err = esp_wifi_scan_get_ap_num(&ap_count);
  if (count_err != ESP_OK) {
    setup_network_scan_running = false;
    ESP_LOGW(TAG, "setup: esp_wifi_scan_get_ap_num failed reason=%s err=%d", reason, static_cast<int>(count_err));
    return false;
  }

  if (ap_count > 40)
    ap_count = 40;

  std::vector<wifi_ap_record_t> records(ap_count);
  esp_err_t records_err = ESP_OK;
  if (ap_count > 0)
    records_err = esp_wifi_scan_get_ap_records(&ap_count, records.data());
  if (records_err != ESP_OK) {
    setup_network_scan_running = false;
    ESP_LOGW(TAG, "setup: esp_wifi_scan_get_ap_records failed reason=%s err=%d", reason,
             static_cast<int>(records_err));
    return false;
  }

  std::vector<NetworkInfo> networks;
  for (uint16_t i = 0; i < ap_count; i++) {
    const auto &record = records[i];
    const size_t ssid_len = strnlen(reinterpret_cast<const char *>(record.ssid), sizeof(record.ssid));
    if (ssid_len == 0)
      continue;

    NetworkInfo network;
    network.ssid.assign(reinterpret_cast<const char *>(record.ssid), ssid_len);
    network.rssi = record.rssi;
    network.locked = record.authmode != WIFI_AUTH_OPEN;
    replace_or_update_network_(&networks, network);
  }

  sort_networks_(&networks);
  setup_network_cache = networks;
  setup_network_cache_ms = millis();
  setup_network_cache_ready = true;
  setup_network_scan_running = false;
  ESP_LOGI(TAG, "setup: cached WiFi networks reason=%s count=%u", reason,
           static_cast<unsigned>(setup_network_cache.size()));
  log_setup_wifi_mode_("after scan finish");
  return true;
#endif
}

void pause_setup_sta_now_() {
  if (wifi::global_wifi_component == nullptr)
    return;
  wifi::global_wifi_component->clear_sta();
#ifdef USE_ESP32
  const auto disconnect_err = esp_wifi_disconnect();
  const auto mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (mode_err != ESP_OK) {
    ESP_LOGW(TAG, "setup: pause STA now failed disconnect=%d mode=%d", static_cast<int>(disconnect_err),
             static_cast<int>(mode_err));
  } else {
    ESP_LOGD(TAG, "setup: pause STA now disconnect=%d", static_cast<int>(disconnect_err));
  }
  log_setup_wifi_mode_("after pause STA");
#endif
}

void RadarApiServer::connect_setup_wifi_deferred(std::string ssid, std::string password) {
  ESP_LOGI("radar_api_server", "setup: connect_setup_wifi_deferred ssid='%s'", ssid.c_str());
  this->cancel_timeout("setup_save_wifi_sta");
  this->defer([ssid, password]() {
    if (wifi::global_wifi_component == nullptr)
      return;

#ifdef USE_ESP32
    wifi_config_t config{};
    if (ssid.empty() || ssid.size() > sizeof(config.sta.ssid) || password.size() > sizeof(config.sta.password)) {
      ESP_LOGE("radar_api_server", "setup: invalid WiFi credential length ssid_len=%u password_len=%u",
               static_cast<unsigned>(ssid.size()), static_cast<unsigned>(password.size()));
      return;
    }

    std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
    if (!password.empty())
      std::memcpy(config.sta.password, password.data(), password.size());
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    wifi_mode_t current_mode = WIFI_MODE_NULL;
    const auto get_mode_err = esp_wifi_get_mode(&current_mode);
    const auto disconnect_err = esp_wifi_disconnect();
    esp_err_t mode_err = ESP_OK;
    if (get_mode_err != ESP_OK || current_mode != WIFI_MODE_APSTA)
      mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    const auto storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    const auto config_err = esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_err_t connect_err = ESP_FAIL;
    if (config_err == ESP_OK)
      connect_err = esp_wifi_connect();

    if (config_err != ESP_OK || connect_err != ESP_OK) {
      ESP_LOGW("radar_api_server",
               "setup: esp-idf connect failed ssid='%s' storage=%d config=%d connect=%d",
               ssid.c_str(), static_cast<int>(storage_err), static_cast<int>(config_err),
               static_cast<int>(connect_err));
    } else {
      ESP_LOGI("radar_api_server", "setup: esp-idf connect started ssid='%s'", ssid.c_str());
    }
    log_setup_wifi_mode_("after setup wifi connect");
#endif
  });
}

void RadarApiServer::pause_setup_sta_deferred() {
  this->cancel_timeout("setup_save_wifi_sta");
  this->defer([]() {
    if (wifi::global_wifi_component == nullptr)
      return;
    wifi::global_wifi_component->clear_sta();
#ifdef USE_ESP32
    const auto disconnect_err = esp_wifi_disconnect();
    const auto mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
      ESP_LOGW("radar_api_server", "setup: pause STA failed disconnect=%d mode=%d",
               static_cast<int>(disconnect_err), static_cast<int>(mode_err));
    }
    log_setup_wifi_mode_("after pause setup STA");
#endif
  });
}

void RadarApiServer::close_setup_ap_deferred() {
  this->cancel_timeout("setup_auto_reboot");
  this->cancel_timeout("setup_ap_close");
  this->defer([]() {
#ifdef USE_ESP32
    log_setup_wifi_mode_("before close_setup_ap");
    const auto err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
      ESP_LOGI("radar_api_server", "setup: setup AP closed");
    } else {
      ESP_LOGW("radar_api_server", "setup: setup AP close failed mode=%d", static_cast<int>(err));
    }
    log_setup_wifi_mode_("after close_setup_ap");
#endif
  });
}

void RadarApiServer::close_setup_ap_after(uint32_t delay_ms, std::function<void()> before_close) {
  this->cancel_timeout("setup_auto_reboot");
  this->cancel_timeout("setup_ap_close");
  this->set_timeout("setup_ap_close", delay_ms, [this, before_close]() {
    ESP_LOGI("radar_api_server", "setup: setup AP grace timeout fired");
    if (before_close)
      before_close();
    this->close_setup_ap_deferred();
  });
}

void RadarApiServer::finish_setup_scan_after(uint32_t delay_ms) {
  this->cancel_timeout("setup_finish_scan");
  this->set_timeout("setup_finish_scan", delay_ms, [this]() { this->setup_handler_.finish_setup_scan(); });
}

void RadarApiServer::finish_setup_scan_and_open_ap_after(uint32_t delay_ms) {
  this->cancel_timeout("setup_finish_scan");
  this->set_timeout("setup_finish_scan", delay_ms, [this]() { this->setup_handler_.finish_setup_scan_and_open_ap(); });
}

void RadarApiServer::open_setup_ap_deferred() {
  this->defer([]() {
#ifdef USE_ESP32
    log_setup_wifi_mode_("before open_setup_ap");
    esp_err_t config_err = ESP_OK;
    const auto mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
#ifdef USE_WIFI_AP
    if (wifi::global_wifi_component != nullptr) {
      const auto ap = wifi::global_wifi_component->get_ap();
      std::string ssid = ap.get_ssid().str();
      std::string password = ap.get_password().str();
      if (ssid.empty()) {
        uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char ssid_buf[29];
        std::snprintf(ssid_buf, sizeof(ssid_buf), "Presence Sensor %02X%02X%02X", mac[3], mac[4], mac[5]);
        ssid = ssid_buf;
      }
      if (!password.empty() && password.size() < 8) {
        ESP_LOGW("radar_api_server", "setup: AP password is too short; using setup fallback password");
        password = "presencesensor";
      }
      wifi_config_t config{};
      if (!ssid.empty() && ssid.size() <= sizeof(config.ap.ssid) && password.size() <= sizeof(config.ap.password)) {
        std::memcpy(config.ap.ssid, ssid.c_str(), ssid.size());
        config.ap.channel = ap.has_channel() ? ap.get_channel() : 1;
        config.ap.ssid_hidden = ap.get_hidden();
        config.ap.max_connection = 5;
        config.ap.beacon_interval = 100;
        if (password.empty()) {
          config.ap.authmode = WIFI_AUTH_OPEN;
        } else {
          config.ap.authmode = WIFI_AUTH_WPA2_PSK;
          std::memcpy(config.ap.password, password.c_str(), password.size());
        }
        config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        config_err = esp_wifi_set_config(WIFI_IF_AP, &config);
      } else {
        config_err = ESP_ERR_INVALID_ARG;
      }
    }
#endif
    if (config_err == ESP_OK && mode_err == ESP_OK) {
      ESP_LOGI("radar_api_server", "setup: setup AP opened");
    } else {
      ESP_LOGW("radar_api_server", "setup: setup AP open issue config=%d mode=%d", static_cast<int>(config_err),
               static_cast<int>(mode_err));
    }
    log_setup_wifi_mode_("after open_setup_ap");
#endif
  });
}

void RadarApiServer::schedule_setup_auto_reboot() {
  ESP_LOGI("radar_api_server", "setup: schedule_setup_auto_reboot delay_ms=300000");
  this->cancel_timeout("setup_auto_reboot");
  this->set_timeout("setup_auto_reboot", 5 * 60 * 1000, []() {
    ESP_LOGW("radar_api_server", "setup: setup_auto_reboot timeout fired");
    App.safe_reboot();
  });
}

void RadarApiServer::reboot_deferred() {
  ESP_LOGI("radar_api_server", "setup: reboot_deferred");
  this->cancel_timeout("setup_auto_reboot");
  this->defer([]() { App.safe_reboot(); });
}

void RadarApiServer::reboot_after(uint32_t delay_ms) {
  this->cancel_timeout("setup_auto_reboot");
  this->cancel_timeout("system_reset_reboot");
  this->set_timeout("system_reset_reboot", delay_ms, []() { App.safe_reboot(); });
}

void RadarApiServer::complete_setup_handoff_after(uint32_t delay_ms) {
  if (this->ha_setup_handoff_started_) {
    ESP_LOGI("radar_api_server", "ha_setup: handoff already started");
    return;
  }
  this->ha_setup_handoff_started_ = true;
  this->cancel_timeout("ha_setup_handoff");
  this->set_timeout("ha_setup_handoff", delay_ms, []() {
    if (wifi::global_wifi_component == nullptr) {
      ESP_LOGE("radar_api_server", "ha_setup: WiFi component unavailable for handoff");
      return;
    }
#ifdef USE_ESP32
    wifi_config_t config{};
    const auto config_err = esp_wifi_get_config(WIFI_IF_STA, &config);
    const auto ssid_len = strnlen(reinterpret_cast<const char *>(config.sta.ssid), sizeof(config.sta.ssid));
    const auto password_len =
        strnlen(reinterpret_cast<const char *>(config.sta.password), sizeof(config.sta.password));
    if (config_err != ESP_OK || ssid_len == 0) {
      ESP_LOGE("radar_api_server", "ha_setup: handoff skipped config=%d ssid_len=%u",
               static_cast<int>(config_err), static_cast<unsigned>(ssid_len));
      return;
    }
    const std::string ssid(reinterpret_cast<const char *>(config.sta.ssid), ssid_len);
    const std::string password(reinterpret_cast<const char *>(config.sta.password), password_len);
    // The setup connection is owned directly by ESP-IDF so the AP remains
    // available while credentials are verified. Persist the same credentials,
    // then reboot so ESPHome owns the runtime STA from the beginning.
    ESP_LOGI("radar_api_server",
             "ha_setup: persist verified WiFi and reboot into ESPHome ownership ssid='%s' password_len=%u",
             ssid.c_str(), static_cast<unsigned>(password_len));
    wifi::global_wifi_component->save_wifi_sta(ssid.c_str(), password.c_str());
    App.safe_reboot();
#endif
  });
}

void RadarApiServer::reset_wifi_credentials_and_reboot_after(uint32_t delay_ms) {
  this->cancel_timeout("setup_auto_reboot");
  this->cancel_timeout("system_reset_reboot");
  this->set_timeout("system_reset_reboot", delay_ms, []() {
    ESP_LOGI("radar_api_server", "wifi_reset: clearing ESPHome saved STA");
    if (wifi::global_wifi_component != nullptr) {
      wifi::global_wifi_component->save_wifi_sta("", "");
      wifi::global_wifi_component->clear_sta();
    }
#ifdef USE_ESP32
    const auto disconnect_err = esp_wifi_disconnect();
    const auto restore_err = esp_wifi_restore();
    ESP_LOGI("radar_api_server", "wifi_reset: esp_wifi_disconnect=%d esp_wifi_restore=%d",
             static_cast<int>(disconnect_err), static_cast<int>(restore_err));
#endif
    App.safe_reboot();
  });
}

}  // namespace radar_api_server
}  // namespace esphome
