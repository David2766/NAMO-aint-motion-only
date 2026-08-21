#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

struct StaticRadarGateTuningState {
  float move_energy{0.0f};
  float still_energy{0.0f};
  float move_threshold{100.0f};
  float still_threshold{100.0f};
};

enum class StaticRadarFactoryResetRequestResult : uint8_t {
  ACCEPTED = 0,
  UNAVAILABLE,
  ALREADY_PENDING,
};

enum class StaticRadarGateRequestResult : uint8_t {
  ACCEPTED = 0,
  BUSY,
};

enum class StaticRadarGateOperationState : uint8_t {
  IDLE = 0,
  PENDING,
  DISPATCHING,
  READBACK_SENT,
  APPLIED,
  FAILED,
};

enum class StaticRadarTuningPhase : uint8_t {
  IDLE = 0,
  STARTING,
  ACTIVE,
  STOPPING,
  FAILED,
};

struct ControlState {
  static constexpr size_t STATIC_RADAR_GATE_COUNT = 9;
  static constexpr uint32_t STATIC_RADAR_READBACK_TIMEOUT_MS = 2000;

  bool status_led_enabled = true;
  bool has_status_led_enabled = false;
  float led_blink_duration = 60.0f;
  bool has_led_blink_duration = false;
  bool environment_correction_enabled = true;
  bool has_environment_correction_enabled = false;
  float temperature_offset = 0.0f;
  bool has_temperature_offset = false;
  float humidity_offset = 0.0f;
  bool has_humidity_offset = false;
  std::string timezone{"Asia/Seoul"};
  bool has_timezone = false;

  bool pending_status_led_enabled = false;
  bool requested_status_led_enabled = true;
  bool pending_led_blink_duration = false;
  float requested_led_blink_duration = 60.0f;
  bool pending_environment_correction_enabled = false;
  bool requested_environment_correction_enabled = true;
  bool pending_temperature_offset = false;
  float requested_temperature_offset = 0.0f;
  bool pending_humidity_offset = false;
  float requested_humidity_offset = 0.0f;
  bool pending_timezone = false;
  std::string requested_timezone{"Asia/Seoul"};

  bool static_radar_available = false;
  bool static_radar_recovering = false;
  bool static_radar_tuning_active = false;
  StaticRadarTuningPhase static_radar_tuning_phase = StaticRadarTuningPhase::IDLE;
  const char *static_radar_tuning_result = "idle";
  bool static_radar_tuning_requested_active = false;
  bool pending_static_radar_tuning_session = false;
  uint32_t static_radar_tuning_keepalive_ms = 0;
  uint16_t static_radar_gate_resolution_mm = 750;
  std::array<StaticRadarGateTuningState, STATIC_RADAR_GATE_COUNT> static_radar_gates{};
  uint8_t requested_static_radar_gate = 0;
  uint8_t requested_static_radar_sensitivity = 0;
  uint32_t static_radar_gate_operation_id = 0;
  uint32_t static_radar_gate_operation_started_ms = 0;
  StaticRadarGateOperationState static_radar_gate_operation_state = StaticRadarGateOperationState::IDLE;
  const char *static_radar_gate_operation_result = "idle";
  float static_radar_gate_actual_move_threshold = NAN;
  float static_radar_gate_actual_still_threshold = NAN;
  bool pending_static_radar_factory_reset = false;

  bool static_radar_gate_operation_busy() const {
    return this->static_radar_gate_operation_state == StaticRadarGateOperationState::PENDING ||
           this->static_radar_gate_operation_state == StaticRadarGateOperationState::DISPATCHING ||
           this->static_radar_gate_operation_state == StaticRadarGateOperationState::READBACK_SENT;
  }

  void mark_static_radar_gate_readback_pending(uint32_t now_ms) {
    if (this->static_radar_gate_operation_state != StaticRadarGateOperationState::DISPATCHING)
      return;
    this->static_radar_gate_operation_started_ms = now_ms;
    this->static_radar_gate_operation_state = StaticRadarGateOperationState::READBACK_SENT;
    this->static_radar_gate_operation_result = "readback_sent";
  }

  void fail_static_radar_gate_operation(const char *result) {
    if (!this->static_radar_gate_operation_busy())
      return;
    this->static_radar_gate_operation_state = StaticRadarGateOperationState::FAILED;
    this->static_radar_gate_operation_result = result;
  }

  StaticRadarGateRequestResult request_static_radar_gate(uint8_t gate, uint8_t sensitivity, uint32_t *operation_id) {
    if (this->static_radar_gate_operation_busy())
      return StaticRadarGateRequestResult::BUSY;

    this->static_radar_gate_operation_id++;
    if (this->static_radar_gate_operation_id == 0)
      this->static_radar_gate_operation_id = 1;
    this->requested_static_radar_gate = gate;
    this->requested_static_radar_sensitivity = sensitivity;
    this->static_radar_gate_operation_started_ms = 0;
    this->static_radar_gate_operation_state = StaticRadarGateOperationState::PENDING;
    this->static_radar_gate_operation_result = "pending";
    this->static_radar_gate_actual_move_threshold = NAN;
    this->static_radar_gate_actual_still_threshold = NAN;
    *operation_id = this->static_radar_gate_operation_id;
    return StaticRadarGateRequestResult::ACCEPTED;
  }

  bool take_static_radar_gate_request(uint8_t *gate, uint8_t *sensitivity) {
    if (this->static_radar_gate_operation_state != StaticRadarGateOperationState::PENDING)
      return false;
    *gate = this->requested_static_radar_gate;
    *sensitivity = this->requested_static_radar_sensitivity;
    this->static_radar_gate_operation_state = StaticRadarGateOperationState::DISPATCHING;
    this->static_radar_gate_operation_result = "dispatching";
    return true;
  }

  bool verify_static_radar_gate_readback(uint32_t now_ms, float move_threshold, float still_threshold) {
    if (this->static_radar_gate_operation_state != StaticRadarGateOperationState::READBACK_SENT)
      return false;

    const bool valid = std::isfinite(move_threshold) && std::isfinite(still_threshold);
    if (!valid && now_ms - this->static_radar_gate_operation_started_ms < STATIC_RADAR_READBACK_TIMEOUT_MS)
      return false;

    this->static_radar_gate_actual_move_threshold = move_threshold;
    this->static_radar_gate_actual_still_threshold = still_threshold;
    const float expected_threshold = 100.0f - this->requested_static_radar_sensitivity;
    const bool matches = valid && std::fabs(move_threshold - expected_threshold) < 0.5f &&
                         std::fabs(still_threshold - expected_threshold) < 0.5f;
    this->static_radar_gate_operation_state =
        matches ? StaticRadarGateOperationState::APPLIED : StaticRadarGateOperationState::FAILED;
    this->static_radar_gate_operation_result =
        matches ? "applied" : (valid ? "readback_mismatch" : "readback_timeout");
    return true;
  }

  bool static_radar_gate_verification_pending(uint8_t *gate) const {
    if (this->static_radar_gate_operation_state != StaticRadarGateOperationState::READBACK_SENT)
      return false;
    *gate = this->requested_static_radar_gate;
    return true;
  }

  StaticRadarFactoryResetRequestResult request_static_radar_factory_reset() {
    if (!this->static_radar_available)
      return StaticRadarFactoryResetRequestResult::UNAVAILABLE;
    if (this->pending_static_radar_factory_reset)
      return StaticRadarFactoryResetRequestResult::ALREADY_PENDING;

    this->static_radar_tuning_requested_active = false;
    this->static_radar_tuning_keepalive_ms = 0;
    this->pending_static_radar_tuning_session = false;
    this->static_radar_gate_operation_state = StaticRadarGateOperationState::IDLE;
    this->static_radar_gate_operation_result = "idle";
    this->static_radar_tuning_active = false;
    this->static_radar_tuning_phase = StaticRadarTuningPhase::IDLE;
    this->static_radar_tuning_result = "factory_reset_pending";
    this->pending_static_radar_factory_reset = true;
    return StaticRadarFactoryResetRequestResult::ACCEPTED;
  }

  bool take_static_radar_factory_reset_request() {
    if (!this->pending_static_radar_factory_reset)
      return false;
    this->pending_static_radar_factory_reset = false;
    return true;
  }
};

}  // namespace radar_api_server
}  // namespace esphome
