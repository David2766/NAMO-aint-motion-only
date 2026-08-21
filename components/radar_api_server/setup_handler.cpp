#include "setup_handler.h"

#include "http_response.h"
#include "radar_api_server.h"
#include "setup_page.h"
#include "setup_security.h"
#include "setup_wifi_manager.h"

#include "esphome/components/api/api_server.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <string>

namespace esphome {
namespace radar_api_server {

void SetupHandler::begin_setup_session_() const {
  const auto before = setup_wifi_stage;
  bool should_open_ap = false;
  if (setup_wifi_stage == SetupWifiStage::IDLE) {
    reset_setup_wifi_state_();
    setup_wifi_stage = SetupWifiStage::READY;
    should_open_ap = true;
  }
  if (before != setup_wifi_stage) {
    ESP_LOGD("radar_setup", "setup: begin_setup_session stage=%s->%s",
             setup_wifi_stage_to_string(before), setup_wifi_stage_to_string(setup_wifi_stage));
  }
  ensure_setup_wifi_events_registered();
  if (should_open_ap) {
    const bool connected = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
    if (!connected) {
      this->server_->pause_setup_sta_deferred();
    }
    this->server_->open_setup_ap_deferred();
  }
}

namespace {

static const char *const TAG = "radar_setup";

std::string json_escape(const char *value) {
  std::string out;
  if (value == nullptr)
    return out;
  for (const char *p = value; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(ch));
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
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

std::string json_escape(const std::string &value) { return json_escape(value.c_str()); }

const char *setup_status_code(bool connected, SetupWifiStage stage) {
  if (connected || stage == SetupWifiStage::CONNECTED)
    return "setup_wifi_connected";
  if (stage == SetupWifiStage::CONNECTING)
    return "setup_wifi_connecting";
  if (stage == SetupWifiStage::FAILED)
    return "setup_wifi_failed";
  return "setup_ready";
}

const char *setup_status_severity(SetupWifiStage stage) {
  return stage == SetupWifiStage::FAILED ? "warning" : "info";
}

}  // namespace

void SetupHandler::prepare_setup_access_point() const {
  if (wifi_connected()) {
    ESP_LOGI(TAG, "setup: prepare AP skipped because STA is connected");
    return;
  }

  const auto before = setup_wifi_stage;
  if (setup_wifi_stage != SetupWifiStage::IDLE) {
    ESP_LOGI(TAG, "setup: prepare AP skipped stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
    return;
  }
  reset_setup_wifi_state_();
  setup_wifi_stage = SetupWifiStage::READY;

  ESP_LOGI(TAG, "setup: prepare access point stage=%s->%s",
           setup_wifi_stage_to_string(before), setup_wifi_stage_to_string(setup_wifi_stage));
  ensure_setup_wifi_events_registered();
  pause_setup_sta_now_();
  const bool scan_started = start_setup_network_scan_from_idf_("pre_ap", true);
  this->server_->finish_setup_scan_and_open_ap_after(scan_started ? 4500 : 1000);
}

void SetupHandler::finish_setup_scan() const {
  if (!setup_network_scan_running) {
    ESP_LOGD(TAG, "setup: finish scan skipped because scan is not running");
    return;
  }
  finish_setup_network_scan_from_idf_(setup_network_scan_reason.empty() ? "finish" : setup_network_scan_reason.c_str());
}

void SetupHandler::finish_setup_scan_and_open_ap() const {
  if (setup_network_scan_running) {
    finish_setup_network_scan_from_idf_(setup_network_scan_reason.empty() ? "finish_open_ap" :
                                                                    setup_network_scan_reason.c_str());
  }
  this->server_->open_setup_ap_deferred();
}

bool SetupHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET) {
    return url == "/" || url == "/setup" || url == "/api/setup/status" || url == "/api/setup/networks" ||
           url == "/api/setup/ping";
  }
  if (request->method() == HTTP_POST) {
    return url == "/api/setup/scan" || url == "/api/setup/wifi" || url == "/api/setup/prepare" ||
           url == "/api/setup/finalize-security" || url == "/api/setup/apply-wifi" ||
           url == "/api/setup/connected" || url == "/api/setup/finish";
  }
  return false;
}

bool SetupHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (request->method() == HTTP_GET && url == "/") {
    ESP_LOGD(TAG, "setup_http: GET /");
    this->handle_root_(request);
    return true;
  }
  if (request->method() == HTTP_GET && url == "/setup") {
    ESP_LOGD(TAG, "setup_http: GET /setup");
    this->handle_setup_page_(request);
    return true;
  }
  if (request->method() == HTTP_GET && url == "/api/setup/status") {
    ESP_LOGD(TAG, "setup_http: GET /api/setup/status");
    this->handle_status_(request);
    return true;
  }
  if (request->method() == HTTP_GET && url == "/api/setup/ping") {
    ESP_LOGD(TAG, "setup_http: GET /api/setup/ping");
    request->send(200, "application/json", "{\"ok\":true}");
    return true;
  }
  if (request->method() == HTTP_GET && url == "/api/setup/networks") {
    ESP_LOGD(TAG, "setup_http: GET /api/setup/networks");
    this->handle_networks_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/setup/scan") {
    ESP_LOGD(TAG, "setup_http: POST /api/setup/scan");
    this->handle_scan_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/setup/wifi") {
    ESP_LOGD(TAG, "setup_http: POST /api/setup/wifi");
    this->handle_apply_wifi_(request);
    return true;
  }
  if (request->method() == HTTP_POST && (url == "/api/setup/prepare" || url == "/api/setup/finalize-security")) {
    ESP_LOGD(TAG, "setup_http: POST %s", url);
    this->handle_prepare_security_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/setup/apply-wifi") {
    ESP_LOGD(TAG, "setup_http: POST /api/setup/apply-wifi");
    this->handle_apply_wifi_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/setup/connected") {
    ESP_LOGD(TAG, "setup_http: POST /api/setup/connected");
    this->handle_connected_(request);
    return true;
  }
  if (request->method() == HTTP_POST && url == "/api/setup/finish") {
    ESP_LOGD(TAG, "setup_http: POST /api/setup/finish");
    this->handle_finish_(request);
    return true;
  }

  ESP_LOGW(TAG, "setup_http: not_found method=%d url=%s", static_cast<int>(request->method()), url);
  request->send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
  return true;
}

void SetupHandler::handle_root_(AsyncWebServerRequest *request) const {
  ESP_LOGD(TAG, "setup: root setup_mode_active=%d stage=%s", setup_mode_active() ? 1 : 0,
           setup_wifi_stage_to_string(setup_wifi_stage));
  if (setup_mode_active()) {
    request->redirect("/setup");
  } else {
    request->redirect("/dashboard");
  }
}

void SetupHandler::handle_setup_page_(AsyncWebServerRequest *request) const {
  ESP_LOGD(TAG, "setup: serve setup page stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
  this->begin_setup_session_();
  auto *response = request->beginResponse(200, "text/html; charset=utf-8", reinterpret_cast<const uint8_t *>(SETUP_PAGE),
                                          SETUP_PAGE_SIZE);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void SetupHandler::handle_status_(AsyncWebServerRequest *request) const {
  this->begin_setup_session_();
  char mac_s[18];
  get_mac_address_pretty_into_buffer(mac_s);
  const bool connected = wifi_connected();
  const bool ap_active = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active();
  const auto name = json_escape(App.get_name());
  std::string current_ip = !setup_wifi_connected_ip.empty() ? setup_wifi_connected_ip : wifi_sta_ip_address();
  if (connected && !current_ip.empty()) {
    setup_wifi_connected_ip = current_ip;
    if (setup_wifi_stage != SetupWifiStage::CONNECTED) {
      ESP_LOGD(TAG, "setup: status recovered stage=%s->connected ip=%s",
               setup_wifi_stage_to_string(setup_wifi_stage), current_ip.c_str());
      setup_wifi_stage = SetupWifiStage::CONNECTED;
    }
    if (setup_wifi_connected_ms == 0)
      setup_wifi_connected_ms = millis();
  } else if (current_ip.empty() && !setup_wifi_connected_ip.empty()) {
    current_ip = setup_wifi_connected_ip;
  }
  const auto ip = json_escape(current_ip);
  const char *key_state = api_key_state();

  if (setup_wifi_stage == SetupWifiStage::CONNECTING) {
    if ((connected && !current_ip.empty()) || !setup_wifi_connected_ip.empty()) {
      setup_wifi_stage = SetupWifiStage::CONNECTED;
      if (setup_wifi_connected_ms == 0)
        setup_wifi_connected_ms = millis();
      ESP_LOGD(TAG, "setup: status promoted connecting->connected ip=%s", current_ip.c_str());
    } else if (setup_wifi_started_ms != 0 && millis() - setup_wifi_started_ms > 90000) {
      setup_wifi_stage = SetupWifiStage::FAILED;
      ESP_LOGW(TAG, "setup: status promoted connecting->failed elapsed_ms=%u disconnect_count=%u",
               millis() - setup_wifi_started_ms, setup_wifi_disconnect_count);
    }
  }

  ESP_LOGD(TAG, "setup: status connected=%d ap_active=%d ip=%s stage=%s ssid='%s' key=%s",
           connected ? 1 : 0, ap_active ? 1 : 0, current_ip.c_str(), setup_wifi_stage_to_string(setup_wifi_stage),
           setup_wifi_ssid.c_str(), key_state);

  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"device\":{\"name\":\"%s\",\"mac\":\"%s\"},"
      "\"wifi\":{\"connected\":%s,\"apActive\":%s,\"ip\":\"%s\"},\"setup\":{\"stage\":\"%s\",\"ssid\":\"%s\"},"
      "\"apiKeyState\":\"%s\",\"statusInfo\":{\"code\":\"%s\",\"severity\":\"%s\",\"detail\":{\"stage\":\"%s\"}}}",
      name.c_str(), mac_s, (connected || setup_wifi_stage == SetupWifiStage::CONNECTED) ? "true" : "false",
      ap_active ? "true" : "false", ip.c_str(),
      setup_wifi_stage_to_string(setup_wifi_stage), json_escape(setup_wifi_ssid).c_str(), key_state,
      setup_status_code(connected, setup_wifi_stage), setup_status_severity(setup_wifi_stage),
      setup_wifi_stage_to_string(setup_wifi_stage));
  request->send(stream);
}

void SetupHandler::handle_networks_(AsyncWebServerRequest *request) const {
  this->begin_setup_session_();
  const auto networks = setup_network_cache;
  const uint32_t age_ms = setup_network_cache_ready ? millis() - setup_network_cache_ms : 0;

  ESP_LOGD(TAG, "setup: networks returning count=%u cache_ready=%d age_ms=%u",
           static_cast<unsigned>(networks.size()), setup_network_cache_ready ? 1 : 0, age_ms);

  auto *stream = request->beginResponseStream("application/json");
  stream->printf("{\"ok\":true,\"cacheReady\":%s,\"scanning\":%s,\"cacheAgeMs\":%u,\"networks\":[",
                 setup_network_cache_ready ? "true" : "false", setup_network_scan_running ? "true" : "false",
                 age_ms);
  bool first = true;
  for (const auto &network : networks) {
    if (!first)
      stream->print(",");
    first = false;
    const auto ssid = json_escape(network.ssid);
    stream->printf("{\"ssid\":\"%s\",\"rssi\":%d,\"locked\":%s}", ssid.c_str(), network.rssi,
                   network.locked ? "true" : "false");
  }
  stream->print("]}");
  request->send(stream);
}

void SetupHandler::handle_scan_(AsyncWebServerRequest *request) const {
  this->begin_setup_session_();
  if (setup_wifi_stage == SetupWifiStage::CONNECTING) {
    http_response::send_error_info(request, 409, "wifi_connecting", "setup_wifi_connecting", "warning",
                                   "{\"stage\":\"connecting\"}");
    return;
  }

  ESP_LOGI(TAG, "setup: esp-idf scan requested by setup UI stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
  const bool ok = start_setup_network_scan_from_idf_("manual", false);
  if (!ok) {
    http_response::send_error_info(request, 500, "scan_failed", "setup_scan_failed", "error", "{}");
    return;
  }
  this->server_->finish_setup_scan_after(3000);
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"scan_started\","
                "\"statusInfo\":{\"code\":\"setup_scan_started\",\"severity\":\"info\",\"detail\":{}}}");
}

void SetupHandler::handle_prepare_security_(AsyncWebServerRequest *request) const {
#ifdef USE_API_NOISE
  if (api::global_api_server == nullptr) {
    http_response::send_error_info(request, 503, "api_key_unavailable", "api_key_unavailable", "error", "{}");
    return;
  }

  const auto dashboard_url = json_escape(std::string("http://") + App.get_name() + ".local/dashboard");
  auto &ctx = api::global_api_server->get_noise_ctx();
  const bool has_psk = ctx.has_psk();
  const bool needs_new_key = !has_psk || psk_equals(ctx.get_psk(), DEMO_API_PSK);
  ESP_LOGI(TAG, "setup: prepare_security has_psk=%d needs_new_key=%d", has_psk ? 1 : 0, needs_new_key ? 1 : 0);

  if (!needs_new_key) {
    ESP_LOGI(TAG, "setup: prepare_security keeping existing api key");
    auto *stream = request->beginResponseStream("application/json");
    stream->printf("{\"ok\":true,\"apiKeyChanged\":false,\"apiKeyState\":\"custom\",\"dashboardUrl\":\"%s\","
                   "\"statusInfo\":{\"code\":\"api_key_ready\",\"severity\":\"info\",\"detail\":{}}}",
                   dashboard_url.c_str());
    request->send(stream);
    return;
  }

  api::psk_t new_psk{};
  if (!generate_api_key(&new_psk)) {
    ESP_LOGE(TAG, "setup: prepare_security random_failed");
    http_response::send_error_info(request, 500, "random_failed", "api_key_random_failed", "error", "{}");
    return;
  }
  if (psk_equals(new_psk, DEMO_API_PSK)) {
    new_psk[0] ^= 0x5A;
  }
  if (!api::global_api_server->save_noise_psk(new_psk, true)) {
    ESP_LOGE(TAG, "setup: prepare_security api_key_save_failed");
    http_response::send_error_info(request, 500, "api_key_save_failed", "api_key_save_failed", "error", "{}");
    return;
  }

  ESP_LOGI(TAG, "setup: prepare_security generated new api key");
  const auto api_key = json_escape(encode_base64_psk(new_psk));
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"apiKeyChanged\":true,\"apiKeyState\":\"custom\",\"apiKey\":\"%s\",\"dashboardUrl\":\"%s\","
      "\"statusInfo\":{\"code\":\"api_key_rotated\",\"severity\":\"info\",\"detail\":{}}}",
      api_key.c_str(), dashboard_url.c_str());
  request->send(stream);
  ESP_LOGI(TAG, "setup: prepare_security scheduling auto reboot guard");
  this->server_->schedule_setup_auto_reboot();
#else
  http_response::send_error_info(request, 503, "api_key_unsupported", "api_key_unsupported", "error", "{}");
#endif
}

void SetupHandler::handle_apply_wifi_(AsyncWebServerRequest *request) const {
  if (wifi::global_wifi_component == nullptr) {
    http_response::send_error_info(request, 503, "wifi_unavailable", "wifi_unavailable", "error", "{}");
    return;
  }

  const auto ssid = request->arg("ssid");
  const auto psk = request->arg("psk");
  if (ssid.empty()) {
    http_response::send_error_info(request, 400, "missing_ssid", "invalid_request", "error", "{}");
    return;
  }
  if (cached_network_is_open_(ssid)) {
    http_response::send_error_info(request, 400, "open_wifi_unsupported", "open_wifi_unsupported", "error", "{}");
    return;
  }
  if (psk.size() < 8 || psk.size() > 63) {
    http_response::send_error_info(request, 400, "invalid_password_length", "invalid_password_length", "error", "{}");
    return;
  }

  ESP_LOGI(TAG, "Applying setup WiFi: SSID='%s'", ssid.c_str());
  ESP_LOGI(TAG, "setup: apply_wifi begin ssid='%s' stage=%s", ssid.c_str(),
           setup_wifi_stage_to_string(setup_wifi_stage));
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"wifi_checking\",\"stage\":\"checking_wifi\","
                "\"statusInfo\":{\"code\":\"setup_wifi_connecting\",\"severity\":\"info\",\"detail\":{\"stage\":\"checking_wifi\"}}}");
  setup_wifi_stage = SetupWifiStage::CONNECTING;
  setup_wifi_ssid = ssid;
  setup_wifi_psk = psk;
  setup_wifi_connected_ip.clear();
  setup_wifi_started_ms = millis();
  setup_wifi_connected_ms = 0;
  setup_wifi_disconnect_count = 0;
  setup_wifi_credentials_saved = false;
  ensure_setup_wifi_events_registered();
  ESP_LOGD(TAG, "setup: apply_wifi set stage=connecting");
  this->server_->connect_setup_wifi_deferred(ssid, psk);
}

void SetupHandler::handle_connected_(AsyncWebServerRequest *request) const {
  ESP_LOGI(TAG, "setup: connected endpoint called stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"setup_ap_close_scheduled\","
                "\"statusInfo\":{\"code\":\"setup_ap_close_scheduled\",\"severity\":\"info\","
                "\"detail\":{\"delayMs\":300000}}}");
  setup_wifi_stage = SetupWifiStage::CONNECTED;
  if (setup_wifi_connected_ip.empty())
    setup_wifi_connected_ip = wifi_sta_ip_address();
  if (setup_wifi_connected_ms == 0)
    setup_wifi_connected_ms = millis();
  this->server_->close_setup_ap_after(5 * 60 * 1000, []() {
    ESP_LOGI(TAG, "setup: connected grace period expired, reset setup stage");
#ifdef USE_ESP32
    persist_setup_wifi_credentials_();
#endif
    reset_setup_wifi_state_();
  });
}

void SetupHandler::handle_finish_(AsyncWebServerRequest *request) const {
  ESP_LOGI(TAG, "setup: finish endpoint called stage=%s", setup_wifi_stage_to_string(setup_wifi_stage));
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"setup_ap_closing\","
                "\"statusInfo\":{\"code\":\"setup_ap_closing\",\"severity\":\"info\",\"detail\":{}}}");
#ifdef USE_ESP32
  persist_setup_wifi_credentials_();
#endif
  reset_setup_wifi_state_();
  ESP_LOGI(TAG, "setup: finish reset stage=idle and close AP");
  this->server_->close_setup_ap_deferred();
}

}  // namespace radar_api_server
}  // namespace esphome
