#pragma once

#include <cstdint>

namespace esphome {
namespace radar_api_server {

struct RoleQuorumState {
  uint8_t expected{0};
  uint8_t online{0};
  uint8_t required{0};
  bool available{false};
};

constexpr uint8_t strict_majority(uint8_t expected) {
  return expected == 0 ? 0 : static_cast<uint8_t>(expected / 2U + 1U);
}

constexpr RoleQuorumState evaluate_role_quorum(uint8_t expected,
                                                uint8_t online) {
  const uint8_t required = strict_majority(expected);
  return {expected, online, required,
          expected != 0 && online >= required};
}

}  // namespace radar_api_server
}  // namespace esphome
