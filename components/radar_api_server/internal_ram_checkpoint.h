#pragma once

#include <cstddef>

namespace esphome {
namespace radar_api_server {

using InternalRamCheckpointCallback =
    void (*)(void *context, const char *source, const char *phase);

class InternalRamCheckpointHook {
 public:
  void set(void *context, InternalRamCheckpointCallback callback) {
    this->context_ = context;
    this->callback_ = callback;
  }

  void record(const char *source, const char *phase) const {
    if (this->callback_ != nullptr)
      this->callback_(this->context_, source, phase);
  }

 private:
  void *context_{nullptr};
  InternalRamCheckpointCallback callback_{nullptr};
};

static_assert(sizeof(InternalRamCheckpointHook) <= sizeof(void *) * 2,
              "RAM checkpoint hook exceeded its fixed budget");

}  // namespace radar_api_server
}  // namespace esphome
