#include "state_handler.h"

#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#endif

#include <new>

namespace esphome {
namespace radar_api_server {

namespace {

static const char *const TAG = "state_handler";
constexpr const char DEFAULT_STATE[] =
    R"({"version":1,"connected":false,"updatedAt":0,"pirMotion":false,"pirMotionEffective":false,"filterBlocked":false,"presence":false,"motion":false,"targetCount":0,"movingTargetCount":0,"stillTargetCount":0,"temperatureC":null,"humidityPercent":null,"illuminanceLux":null,"targets":[]})";

#ifdef USE_ESP32
portMUX_TYPE state_response_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

}  // namespace

struct StateResponseWorkspace {
  static constexpr std::size_t STATE_BUFFER_SIZE = 8192;
  static constexpr std::size_t TRACKER_BUFFER_SIZE = 2048;

  char state[2][STATE_BUFFER_SIZE]{};
  char tracker[TRACKER_BUFFER_SIZE]{};
  std::size_t state_size[2]{};
  uint8_t readers[2]{};
  uint8_t active_index{0};
  bool published{false};
  StatePublisherDiagnostics diagnostics{};
};

static_assert(sizeof(StateResponseWorkspace) <= 18464,
              "State response workspace exceeded its PSRAM budget");

StateHandler::~StateHandler() {
  if (this->workspace_ == nullptr)
    return;
#if defined(USE_ESP32) && defined(USE_PSRAM)
  this->workspace_->~StateResponseWorkspace();
  heap_caps_free(this->workspace_);
#else
  delete this->workspace_;
#endif
  this->workspace_ = nullptr;
}

void StateHandler::setup() {
  if (this->workspace_ != nullptr)
    return;
#if defined(USE_ESP32) && defined(USE_PSRAM)
  void *memory = heap_caps_calloc(1, sizeof(StateResponseWorkspace),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    this->workspace_ = new (memory) StateResponseWorkspace();
    ESP_LOGI(TAG,
             "State response workspace allocated in PSRAM size=%u "
             "state_capacity=%u tracker_capacity=%u",
             static_cast<unsigned>(sizeof(StateResponseWorkspace)),
             static_cast<unsigned>(StateResponseWorkspace::STATE_BUFFER_SIZE),
             static_cast<unsigned>(StateResponseWorkspace::TRACKER_BUFFER_SIZE));
  } else {
    ESP_LOGE(TAG,
             "State response workspace PSRAM allocation failed size=%u; "
             "live state publishing unavailable",
             static_cast<unsigned>(sizeof(StateResponseWorkspace)));
  }
#elif !defined(USE_ESP32)
  this->workspace_ = new (std::nothrow) StateResponseWorkspace();
#else
  ESP_LOGE(TAG,
           "State response workspace requires PSRAM; live state publishing "
           "unavailable");
#endif
}

StateUpdateLease StateHandler::begin_update() {
  StateUpdateLease lease;
  if (this->workspace_ == nullptr)
    return lease;

#ifdef USE_ESP32
  portENTER_CRITICAL(&state_response_mux);
#endif
  ++this->workspace_->diagnostics.attempted;
  const uint8_t buffer_index = this->workspace_->published
                                   ? this->workspace_->active_index ^ 1U
                                   : 0U;
  if (this->workspace_->readers[buffer_index] == 0) {
    lease.state = this->workspace_->state[buffer_index];
    lease.state_capacity = StateResponseWorkspace::STATE_BUFFER_SIZE;
    lease.tracker = this->workspace_->tracker;
    lease.tracker_capacity = StateResponseWorkspace::TRACKER_BUFFER_SIZE;
    lease.buffer_index = buffer_index;
  } else {
    ++this->workspace_->diagnostics.skipped_busy_buffer;
  }
#ifdef USE_ESP32
  portEXIT_CRITICAL(&state_response_mux);
#endif
  return lease;
}

bool StateHandler::commit_update(const StateUpdateLease &lease,
                                 std::size_t state_size) {
  if (this->workspace_ == nullptr || !lease.valid() ||
      lease.buffer_index >= 2 || state_size == 0 ||
      state_size >= StateResponseWorkspace::STATE_BUFFER_SIZE ||
      lease.state != this->workspace_->state[lease.buffer_index]) {
    if (this->workspace_ != nullptr) {
#ifdef USE_ESP32
      portENTER_CRITICAL(&state_response_mux);
#endif
      ++this->workspace_->diagnostics.failed_commit;
#ifdef USE_ESP32
      portEXIT_CRITICAL(&state_response_mux);
#endif
    }
    return false;
  }

#ifdef USE_ESP32
  portENTER_CRITICAL(&state_response_mux);
#endif
  this->workspace_->state_size[lease.buffer_index] = state_size;
  this->workspace_->active_index = lease.buffer_index;
  this->workspace_->published = true;
  ++this->workspace_->diagnostics.published;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&state_response_mux);
#endif
  return true;
}

StatePublisherDiagnostics StateHandler::diagnostics() const {
  StatePublisherDiagnostics diagnostics{};
  if (this->workspace_ == nullptr)
    return diagnostics;
#ifdef USE_ESP32
  portENTER_CRITICAL(&state_response_mux);
#endif
  diagnostics = this->workspace_->diagnostics;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&state_response_mux);
#endif
  return diagnostics;
}

bool StateHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() != HTTP_GET)
    return false;
  return url == "/api/state";
}

bool StateHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  this->handle_get_state_(request);
  return true;
}

void StateHandler::handle_get_state_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  const char *state = DEFAULT_STATE;
  std::size_t state_size = sizeof(DEFAULT_STATE) - 1;
  uint8_t buffer_index = 0;
  bool acquired = false;

  portENTER_CRITICAL(&state_response_mux);
  if (this->workspace_ != nullptr && this->workspace_->published) {
    buffer_index = this->workspace_->active_index;
    this->workspace_->readers[buffer_index]++;
    state = this->workspace_->state[buffer_index];
    state_size = this->workspace_->state_size[buffer_index];
    acquired = true;
  }
  portEXIT_CRITICAL(&state_response_mux);

  httpd_req_t *raw_request = *request;
  httpd_resp_set_status(raw_request, HTTPD_200);
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Accept-Ranges", "none");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  const esp_err_t result =
      httpd_resp_send(raw_request, state, static_cast<ssize_t>(state_size));

  if (acquired) {
    portENTER_CRITICAL(&state_response_mux);
    this->workspace_->readers[buffer_index]--;
    portEXIT_CRITICAL(&state_response_mux);
  }
  if (result != ESP_OK)
    ESP_LOGW(TAG, "State response streaming failed");
#else
  request->send(200, "application/json", DEFAULT_STATE);
#endif
}

}  // namespace radar_api_server
}  // namespace esphome
