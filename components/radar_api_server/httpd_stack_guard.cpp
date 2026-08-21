#include "esphome/core/log.h"

#include <cstddef>

#ifdef USE_ESP32
#include "esp_http_server.h"

namespace {

static const char *const TAG = "httpd_stack_guard";
// Participant enrollment persists several bounded registries before returning
// the full participant snapshot. ESP-IDF's HTTP response path exhausted the
// previous 6 KiB task stack, so reserve room for the seven-device Group path.
static constexpr size_t NAMO_HTTPD_STACK_SIZE = 8192;

}  // namespace

extern "C" esp_err_t __real_httpd_start(
    httpd_handle_t *handle, const httpd_config_t *config);

extern "C" esp_err_t __wrap_httpd_start(
    httpd_handle_t *handle, const httpd_config_t *config) {
  if (config == nullptr)
    return __real_httpd_start(handle, config);

  httpd_config_t adjusted = *config;
  const size_t requested_stack_size = adjusted.stack_size;
  if (adjusted.stack_size < NAMO_HTTPD_STACK_SIZE)
    adjusted.stack_size = NAMO_HTTPD_STACK_SIZE;

  ESP_LOGI(TAG,
           "HTTPD task stack configured requested=%u applied=%u "
           "region=internal",
           static_cast<unsigned>(requested_stack_size),
           static_cast<unsigned>(adjusted.stack_size));
  return __real_httpd_start(handle, &adjusted);
}
#endif
