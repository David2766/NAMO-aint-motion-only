#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radar_api_server {

enum class Ld2410StreamAction : uint8_t {
  NONE = 0,
  RX_STARTED,
  STARTUP_HEALTHY,
  STARTUP_NO_DATA,
  STARTUP_NO_VALID_FRAME,
  STREAM_STALLED_SILENT,
  STREAM_STALLED_INVALID,
  STREAM_RESUMED,
  UNHEALTHY_SUMMARY,
};

constexpr uint8_t LD2410_COMMAND_ENABLE_CONFIGURATION = 0xFF;
constexpr uint8_t LD2410_COMMAND_DISABLE_CONFIGURATION = 0xFE;
constexpr uint8_t LD2410_COMMAND_RESTART = 0xA3;
constexpr uint8_t LD2410_ACK_HISTORY_CAPACITY = 8;

struct Ld2410AckEvent {
  uint32_t sequence{0};
  uint16_t result{0};
  uint8_t command{0};
};

static_assert(sizeof(Ld2410AckEvent) <= 8,
              "LD2410 ACK event must remain compact");

struct Ld2410StreamSnapshot {
  uint32_t rx_bytes{0};
  uint32_t periodic_frames{0};
  uint32_t ack_frames{0};
  uint32_t failed_ack_frames{0};
  uint32_t ack_sequence{0};
  uint32_t invalid_frames{0};
  uint32_t discarded_bytes{0};
  uint32_t first_byte_delay_ms{0};
  uint32_t first_frame_delay_ms{0};
  uint32_t last_byte_age_ms{0};
  uint32_t last_frame_age_ms{0};
  uint32_t stall_duration_ms{0};
  uint16_t moving_distance_cm{0};
  uint16_t still_distance_cm{0};
  uint16_t detection_distance_cm{0};
  uint8_t moving_energy{0};
  uint8_t still_energy{0};
  uint8_t target_state{0};
  uint8_t last_ack_command{0};
  uint16_t last_ack_result{0};
  Ld2410AckEvent ack_events[LD2410_ACK_HISTORY_CAPACITY]{};
  uint8_t ack_event_count{0};
  bool engineering_mode{false};
  bool rx_seen{false};
  bool valid_frame_seen{false};
  bool stalled{false};
  bool ack_seen{false};
  bool configuration_mode_open{false};
};

enum class Ld2410RecoveryAction : uint8_t {
  NONE = 0,
  ENTER_CONFIGURATION,
  RESTART,
  RECOVERED,
  GIVE_UP,
};

enum class Ld2410ManagedOperation : uint8_t {
  NONE = 0,
  STARTUP_SYNC,
  TUNING_START,
  TUNING_STOP,
  GATE_WRITE,
  FACTORY_RESET,
};

enum class Ld2410ManagedEvent : uint8_t {
  NONE = 0,
  SEND_COMMAND,
  COMPLETED,
  FAILED,
};

enum class Ld2410ManagedFailure : uint8_t {
  NONE = 0,
  ACK_REJECTED,
  ACK_TIMEOUT,
  UNEXPECTED_ACK,
  FRAME_TIMEOUT,
};

struct Ld2410ManagedAction {
  static constexpr std::size_t MAX_PAYLOAD_SIZE = 18;

  Ld2410ManagedEvent event{Ld2410ManagedEvent::NONE};
  Ld2410ManagedOperation operation{Ld2410ManagedOperation::NONE};
  Ld2410ManagedFailure failure{Ld2410ManagedFailure::NONE};
  uint8_t command{0};
  uint16_t ack_result{0};
  uint8_t ignored_acks{0};
  uint8_t last_ignored_command{0};
  uint16_t last_ignored_result{0};
  uint8_t payload[MAX_PAYLOAD_SIZE]{};
  uint8_t payload_size{0};
};

const char *ld2410_managed_operation_name(Ld2410ManagedOperation operation);
const char *ld2410_managed_failure_name(Ld2410ManagedFailure failure);

class Ld2410CommandTransaction {
 public:
  static constexpr uint32_t ACK_TIMEOUT_MS = 1500;
  static constexpr uint32_t FRAME_TIMEOUT_MS = 5000;
  static constexpr uint32_t RESTART_FRAME_TIMEOUT_MS = 10000;

  void reset();
  bool start(Ld2410ManagedOperation operation, uint32_t now_ms,
             const Ld2410StreamSnapshot &stream, uint8_t gate = 0,
             uint8_t sensitivity = 0);
  Ld2410ManagedAction update(uint32_t now_ms,
                             const Ld2410StreamSnapshot &stream);

  bool busy() const { return this->operation_ != Ld2410ManagedOperation::NONE; }
  Ld2410ManagedOperation operation() const { return this->operation_; }

 private:
  enum class State : uint8_t {
    IDLE = 0,
    SEND_NEXT,
    WAIT_ACK,
    WAIT_FRAME,
    SEND_ABORT_EXIT,
    REPORT_FAILURE,
  };

  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
  static bool sequence_after_(uint32_t sequence, uint32_t baseline);
  uint8_t sequence_length_() const;
  bool command_for_step_(uint8_t step, Ld2410ManagedAction *action) const;
  bool expected_engineering_mode_() const;
  void begin_failure_(Ld2410ManagedFailure failure, uint16_t ack_result = 0);
  Ld2410ManagedAction finish_(Ld2410ManagedEvent event,
                              Ld2410ManagedFailure failure);

  State state_{State::IDLE};
  Ld2410ManagedOperation operation_{Ld2410ManagedOperation::NONE};
  Ld2410ManagedFailure pending_failure_{Ld2410ManagedFailure::NONE};
  uint32_t deadline_ms_{0};
  uint32_t ack_cursor_{0};
  uint32_t frame_baseline_{0};
  uint8_t expected_command_{0};
  uint8_t failure_command_{0};
  uint8_t step_{0};
  uint8_t gate_{0};
  uint8_t sensitivity_{0};
  uint16_t failure_ack_result_{0};
  uint8_t ignored_ack_count_{0};
  uint8_t last_ignored_command_{0};
  uint16_t last_ignored_result_{0};
  bool aborting_{false};
};

class Ld2410Watchdog {
 public:
  static constexpr uint32_t STARTUP_STALE_MS = 5000;
  static constexpr uint32_t RUNTIME_STALE_MS = 10000;
  static constexpr uint32_t ACK_TIMEOUT_MS = 1500;
  static constexpr uint32_t RESTART_TIMEOUT_MS = 10000;
  static constexpr uint32_t RETRY_COOLDOWN_MS = 60000;

  void reset(uint32_t now_ms);
  Ld2410RecoveryAction update(uint32_t now_ms,
                              const Ld2410StreamSnapshot &stream,
                              bool command_busy,
                              bool force_recovery = false);

  uint8_t attempts() const { return this->attempts_; }

 private:
  enum class State : uint8_t {
    MONITORING = 0,
    WAIT_ENABLE_ACK,
    WAIT_FRAME,
    COOLDOWN,
  };

  static bool time_reached_(uint32_t now_ms, uint32_t deadline_ms);
  static bool sequence_after_(uint32_t sequence, uint32_t baseline);
  bool received_successful_ack_(const Ld2410StreamSnapshot &stream,
                                uint8_t command);
  bool received_recovery_frame_(const Ld2410StreamSnapshot &stream) const;
  Ld2410RecoveryAction begin_recovery_(
      uint32_t now_ms, const Ld2410StreamSnapshot &stream);

  State state_{State::MONITORING};
  uint32_t deadline_ms_{0};
  uint32_t periodic_baseline_{0};
  uint32_t ack_cursor_{0};
  uint8_t attempts_{0};
  bool restart_sent_{false};
};

class Ld2410StreamHealth {
 public:
  static constexpr uint32_t STARTUP_GRACE_MS = 5000;
  static constexpr uint32_t RUNTIME_STALE_MS = 5000;
  static constexpr uint32_t UNHEALTHY_REPORT_INTERVAL_MS = 30000;

  void reset(uint32_t now_ms);
  void use_canonical_ack_source() { this->canonical_ack_source_ = true; }
  void observe_ack(uint8_t command, uint16_t result, uint32_t now_ms);
  void observe_restart_settled() { this->configuration_mode_open_ = false; }
  void observe_rx_byte(uint8_t byte, uint32_t now_ms);
  Ld2410StreamAction update(uint32_t now_ms);
  Ld2410StreamSnapshot snapshot(uint32_t now_ms) const;

 private:
  enum class FrameKind : uint8_t {
    NONE = 0,
    PERIODIC,
    ACK,
  };

  static constexpr std::size_t HEADER_SIZE = 4;
  static constexpr std::size_t MAX_FRAME_SIZE = 50;

  void observe_idle_byte_(uint8_t byte);
  void append_frame_byte_(uint8_t byte, uint32_t now_ms);
  void complete_frame_(uint32_t now_ms);
  void reset_frame_();
  static uint8_t advance_header_match_(const uint8_t *header, uint8_t matched,
                                       uint8_t byte);
  static bool ends_with_(const uint8_t *data, std::size_t size,
                         const uint8_t *suffix, std::size_t suffix_size);
  static uint16_t decode_u16_(uint8_t low, uint8_t high);

  uint8_t frame_[MAX_FRAME_SIZE]{};
  std::size_t frame_size_{0};
  FrameKind frame_kind_{FrameKind::NONE};
  uint8_t periodic_header_match_{0};
  uint8_t ack_header_match_{0};

  uint32_t boot_ms_{0};
  uint32_t first_byte_ms_{0};
  uint32_t first_valid_frame_ms_{0};
  uint32_t last_byte_ms_{0};
  uint32_t last_valid_frame_ms_{0};
  uint32_t stall_started_ms_{0};
  uint32_t last_stall_duration_ms_{0};
  uint32_t last_unhealthy_report_ms_{0};
  uint32_t rx_bytes_at_last_valid_frame_{0};
  uint32_t rx_bytes_{0};
  uint32_t periodic_frames_{0};
  uint32_t ack_frames_{0};
  uint32_t failed_ack_frames_{0};
  uint32_t ack_sequence_{0};
  uint32_t invalid_frames_{0};
  uint32_t discarded_bytes_{0};

  uint16_t moving_distance_cm_{0};
  uint16_t still_distance_cm_{0};
  uint16_t detection_distance_cm_{0};
  uint8_t moving_energy_{0};
  uint8_t still_energy_{0};
  uint8_t target_state_{0};
  uint8_t last_ack_command_{0};
  uint16_t last_ack_result_{0};
  Ld2410AckEvent ack_events_[LD2410_ACK_HISTORY_CAPACITY]{};
  uint8_t ack_event_head_{0};
  uint8_t ack_event_count_{0};
  bool engineering_mode_{false};

  bool rx_seen_{false};
  bool valid_frame_seen_{false};
  bool startup_reported_{false};
  bool healthy_ever_{false};
  bool stalled_{false};
  bool pending_rx_started_{false};
  bool pending_first_valid_frame_{false};
  bool pending_stream_resumed_{false};
  bool canonical_ack_source_{false};
  bool ack_seen_{false};
  bool configuration_mode_open_{false};
};

static_assert(sizeof(Ld2410StreamHealth) <= 312,
              "LD2410 stream health monitor must remain bounded");
static_assert(sizeof(Ld2410Watchdog) <= 24,
              "LD2410 watchdog must remain bounded");
static_assert(sizeof(Ld2410CommandTransaction) <= 48,
              "LD2410 command transaction must remain bounded");

}  // namespace radar_api_server
}  // namespace esphome
