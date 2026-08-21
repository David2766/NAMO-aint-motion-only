#pragma once

#include "esphome/components/web_server_base/web_server_base.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

struct StateResponseWorkspace;

struct StateUpdateLease {
  char *state{nullptr};
  std::size_t state_capacity{0};
  char *tracker{nullptr};
  std::size_t tracker_capacity{0};
  uint8_t buffer_index{0};

  bool valid() const {
    return this->state != nullptr && this->tracker != nullptr;
  }
};

struct StatePublisherDiagnostics {
  uint32_t attempted{0};
  uint32_t published{0};
  uint32_t skipped_busy_buffer{0};
  uint32_t failed_commit{0};
};

class StateHandler {
 public:
  ~StateHandler();

  void setup();
  StateUpdateLease begin_update();
  bool commit_update(const StateUpdateLease &lease, std::size_t state_size);
  StatePublisherDiagnostics diagnostics() const;

  bool can_handle(AsyncWebServerRequest *request, StringRef url) const;
  bool handle(AsyncWebServerRequest *request, StringRef url);

 private:
  StateResponseWorkspace *workspace_{nullptr};

  void handle_get_state_(AsyncWebServerRequest *request);
};

}  // namespace radar_api_server
}  // namespace esphome
