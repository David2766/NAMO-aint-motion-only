#include "ld2410_stream_health.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

using esphome::radar_api_server::Ld2410StreamAction;
using esphome::radar_api_server::Ld2410StreamHealth;
using esphome::radar_api_server::Ld2410RecoveryAction;
using esphome::radar_api_server::Ld2410Watchdog;
using esphome::radar_api_server::Ld2410CommandTransaction;
using esphome::radar_api_server::Ld2410ManagedEvent;
using esphome::radar_api_server::Ld2410ManagedFailure;
using esphome::radar_api_server::Ld2410ManagedOperation;
using esphome::radar_api_server::Ld2410StreamSnapshot;
using esphome::radar_api_server::LD2410_ACK_HISTORY_CAPACITY;
using esphome::radar_api_server::LD2410_COMMAND_DISABLE_CONFIGURATION;
using esphome::radar_api_server::LD2410_COMMAND_ENABLE_CONFIGURATION;

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "ld2410_stream_health_tests: %s\n", message);
  std::exit(1);
}

void require(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

std::vector<uint8_t> periodic_frame(uint8_t target_state = 0x02,
                                    uint16_t moving_distance_cm = 123,
                                    uint8_t moving_energy = 45,
                                    uint16_t still_distance_cm = 234,
                                    uint8_t still_energy = 67,
                                    uint16_t detection_distance_cm = 345) {
  return {
      0xF4,
      0xF3,
      0xF2,
      0xF1,
      0x0D,
      0x00,
      0x02,
      0xAA,
      target_state,
      static_cast<uint8_t>(moving_distance_cm & 0xFFU),
      static_cast<uint8_t>(moving_distance_cm >> 8U),
      moving_energy,
      static_cast<uint8_t>(still_distance_cm & 0xFFU),
      static_cast<uint8_t>(still_distance_cm >> 8U),
      still_energy,
      static_cast<uint8_t>(detection_distance_cm & 0xFFU),
      static_cast<uint8_t>(detection_distance_cm >> 8U),
      0x55,
      0x00,
      0xF8,
      0xF7,
      0xF6,
      0xF5,
  };
}

std::vector<uint8_t> ack_frame(uint8_t command = 0xFF,
                               uint16_t result = 0) {
  return {
      0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, command,
      0x01, static_cast<uint8_t>(result & 0xFFU),
      static_cast<uint8_t>(result >> 8U), 0x04, 0x03, 0x02, 0x01,
  };
}

void feed(Ld2410StreamHealth &health, const std::vector<uint8_t> &bytes,
          uint32_t now_ms) {
  for (uint8_t byte : bytes)
    health.observe_rx_byte(byte, now_ms);
}

void test_silent_startup() {
  Ld2410StreamHealth health;
  health.reset(100);
  require(health.update(5099) == Ld2410StreamAction::NONE,
          "startup grace must not expire early");
  require(health.update(5100) == Ld2410StreamAction::STARTUP_NO_DATA,
          "silent startup must be distinguished from invalid traffic");
  const auto snapshot = health.snapshot(5100);
  require(!snapshot.rx_seen && snapshot.rx_bytes == 0,
          "silent startup must report zero RX bytes");
}

void test_healthy_startup_and_values() {
  Ld2410StreamHealth health;
  health.reset(100);
  feed(health, ack_frame(), 130);
  feed(health, periodic_frame(), 180);

  require(health.update(180) == Ld2410StreamAction::RX_STARTED,
          "first received byte must be reported");
  require(health.update(180) == Ld2410StreamAction::STARTUP_HEALTHY,
          "first valid periodic frame must mark startup healthy");

  const auto snapshot = health.snapshot(180);
  require(snapshot.periodic_frames == 1 && snapshot.ack_frames == 1,
          "periodic and ACK frames must be counted independently");
  require(snapshot.first_byte_delay_ms == 30 &&
              snapshot.first_frame_delay_ms == 80,
          "startup delays must be measured from monitor reset");
  require(snapshot.target_state == 0x02 &&
              snapshot.moving_distance_cm == 123 &&
              snapshot.moving_energy == 45 &&
              snapshot.still_distance_cm == 234 &&
              snapshot.still_energy == 67 &&
              snapshot.detection_distance_cm == 345,
          "latest valid periodic evidence must be decoded");
}

void test_invalid_startup_traffic() {
  Ld2410StreamHealth health;
  health.reset(0);
  feed(health, ack_frame(), 100);
  feed(health, {0x10, 0x20, 0x30, 0x40}, 200);
  require(health.update(200) == Ld2410StreamAction::RX_STARTED,
          "invalid startup traffic must still report RX start");
  require(health.update(5000) ==
              Ld2410StreamAction::STARTUP_NO_VALID_FRAME,
          "ACK and noise without periodic frames must be reported separately");
  const auto snapshot = health.snapshot(5000);
  require(snapshot.ack_frames == 1 && snapshot.periodic_frames == 0 &&
              snapshot.discarded_bytes >= 4,
          "invalid startup summary must retain ACK and noise counters");
}

void test_stall_and_resume() {
  Ld2410StreamHealth health;
  health.reset(0);
  feed(health, periodic_frame(), 100);
  require(health.update(100) == Ld2410StreamAction::RX_STARTED,
          "healthy stream must first report RX start");
  require(health.update(100) == Ld2410StreamAction::STARTUP_HEALTHY,
          "healthy stream must report startup success");

  require(health.update(5100) ==
              Ld2410StreamAction::STREAM_STALLED_SILENT,
          "a silent UART after a healthy frame must report a silent stall");
  feed(health, periodic_frame(0x01), 5300);
  require(health.update(5300) == Ld2410StreamAction::STREAM_RESUMED,
          "a valid frame after a stall must report recovery");
  const auto snapshot = health.snapshot(5300);
  require(snapshot.stall_duration_ms == 200 && !snapshot.stalled &&
              snapshot.periodic_frames == 2,
          "recovery must preserve the completed stall duration");
}

void test_invalid_bytes_during_stall() {
  Ld2410StreamHealth health;
  health.reset(0);
  feed(health, periodic_frame(), 100);
  require(health.update(100) == Ld2410StreamAction::RX_STARTED,
          "test precondition RX action missing");
  require(health.update(100) == Ld2410StreamAction::STARTUP_HEALTHY,
          "test precondition healthy action missing");
  feed(health, {0x11, 0x22, 0x33}, 5100);
  require(health.update(5100) ==
              Ld2410StreamAction::STREAM_STALLED_INVALID,
          "continuing bytes without valid frames must not look like silence");
}

void test_periodic_unhealthy_summary() {
  Ld2410StreamHealth health;
  health.reset(0);
  require(health.update(5000) == Ld2410StreamAction::STARTUP_NO_DATA,
          "test precondition startup failure missing");
  require(health.update(34999) == Ld2410StreamAction::NONE,
          "unhealthy summary must be rate limited");
  require(health.update(35000) ==
              Ld2410StreamAction::UNHEALTHY_SUMMARY,
          "persistent failure must produce a bounded periodic summary");
}

void test_millis_rollover() {
  Ld2410StreamHealth health;
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 1000U;
  health.reset(start);
  feed(health, periodic_frame(), start + 500U);
  require(health.update(start + 500U) == Ld2410StreamAction::RX_STARTED,
          "rollover test RX action missing");
  require(health.update(start + 500U) ==
              Ld2410StreamAction::STARTUP_HEALTHY,
          "rollover test healthy action missing");
  require(health.update(start + 5500U) ==
              Ld2410StreamAction::STREAM_STALLED_SILENT,
          "unsigned elapsed time must remain correct across millis rollover");
}

void test_ack_session_diagnostics() {
  Ld2410StreamHealth health;
  health.reset(0);
  feed(health, ack_frame(LD2410_COMMAND_ENABLE_CONFIGURATION), 100);
  auto snapshot = health.snapshot(100);
  require(snapshot.ack_frames == 1 && snapshot.failed_ack_frames == 0 &&
              snapshot.last_ack_command ==
                  LD2410_COMMAND_ENABLE_CONFIGURATION &&
              snapshot.last_ack_result == 0 &&
              snapshot.configuration_mode_open,
          "successful enable ACK must open the tracked command session");

  feed(health, ack_frame(0xA0, 1), 120);
  snapshot = health.snapshot(120);
  require(snapshot.failed_ack_frames == 1 && snapshot.invalid_frames == 1 &&
              snapshot.last_ack_command == 0xA0 &&
              snapshot.last_ack_result == 1 &&
              snapshot.configuration_mode_open,
          "failed command ACK must retain its command, result, and session");

  feed(health, ack_frame(LD2410_COMMAND_DISABLE_CONFIGURATION), 140);
  snapshot = health.snapshot(140);
  require(snapshot.ack_frames == 2 && !snapshot.configuration_mode_open &&
              snapshot.ack_sequence == 3 && snapshot.ack_event_count == 3 &&
              snapshot.ack_events[0].command ==
                  LD2410_COMMAND_ENABLE_CONFIGURATION &&
              snapshot.ack_events[1].command == 0xA0 &&
              snapshot.ack_events[1].result == 1 &&
              snapshot.ack_events[2].command ==
                  LD2410_COMMAND_DISABLE_CONFIGURATION,
          "successful disable ACK must close the tracked command session");
}

void test_ack_history_is_bounded_and_ordered() {
  Ld2410StreamHealth health;
  health.reset(0);
  for (uint8_t command = 1; command <= 10; command++)
    feed(health, ack_frame(command), command * 10);
  const auto snapshot = health.snapshot(100);
  require(snapshot.ack_sequence == 10 &&
              snapshot.ack_event_count == LD2410_ACK_HISTORY_CAPACITY &&
              snapshot.ack_events[0].sequence == 3 &&
              snapshot.ack_events[0].command == 3 &&
              snapshot.ack_events[LD2410_ACK_HISTORY_CAPACITY - 1].sequence ==
                  10 &&
              snapshot.ack_events[LD2410_ACK_HISTORY_CAPACITY - 1].command ==
                  10,
          "ACK history must retain the newest events in receive order");
}

void test_canonical_ack_source_is_not_double_counted() {
  Ld2410StreamHealth health;
  health.reset(0);
  health.use_canonical_ack_source();
  feed(health, ack_frame(0x64, 1), 100);
  auto snapshot = health.snapshot(100);
  require(snapshot.ack_sequence == 0 && snapshot.ack_frames == 0 &&
              snapshot.failed_ack_frames == 0,
          "raw UART tap must not duplicate canonical component ACKs");

  health.observe_ack(0x64, 1, 101);
  snapshot = health.snapshot(101);
  require(snapshot.ack_sequence == 1 && snapshot.failed_ack_frames == 1 &&
              snapshot.last_ack_command == 0x64 &&
              snapshot.last_ack_result == 1,
          "canonical component ACK must preserve the real command result");
}

void test_watchdog_restarts_open_stalled_session() {
  Ld2410StreamHealth health;
  Ld2410Watchdog watchdog;
  health.reset(0);
  watchdog.reset(0);

  feed(health, periodic_frame(), 100);
  feed(health, periodic_frame(), 200);
  feed(health, ack_frame(LD2410_COMMAND_ENABLE_CONFIGURATION), 250);
  auto snapshot = health.snapshot(10199);
  require(watchdog.update(10199, snapshot, false) ==
              Ld2410RecoveryAction::NONE,
          "runtime watchdog must honor its stale threshold");
  snapshot = health.snapshot(10200);
  require(watchdog.update(10200, snapshot, false) ==
              Ld2410RecoveryAction::RESTART,
          "an already open command session must restart directly");

  feed(health, periodic_frame(), 10300);
  snapshot = health.snapshot(10300);
  require(watchdog.update(10300, snapshot, false) ==
              Ld2410RecoveryAction::RECOVERED,
          "only a new periodic frame may complete watchdog recovery");
  require(watchdog.attempts() == 1 && snapshot.configuration_mode_open,
          "periodic telemetry alone must not forge a configuration close");
  health.observe_restart_settled();
  require(!health.snapshot(10300).configuration_mode_open,
          "the command manager must close the tracked session after settling");
}

void test_watchdog_restart_and_cooldown() {
  Ld2410StreamHealth health;
  Ld2410Watchdog watchdog;
  health.reset(0);
  watchdog.reset(0);

  auto snapshot = health.snapshot(5000);
  require(watchdog.update(5000, snapshot, false) ==
              Ld2410RecoveryAction::ENTER_CONFIGURATION,
          "silent startup must open one bounded command session");
  feed(health, ack_frame(LD2410_COMMAND_ENABLE_CONFIGURATION), 5100);
  snapshot = health.snapshot(5100);
  require(watchdog.update(5100, snapshot, false) ==
              Ld2410RecoveryAction::RESTART,
          "restart must only be sent after a successful enable ACK");
  require(watchdog.update(15099, snapshot, false) ==
              Ld2410RecoveryAction::NONE,
          "restart recovery must wait its bounded timeout");
  require(watchdog.update(15100, snapshot, false) ==
              Ld2410RecoveryAction::GIVE_UP,
          "a failed restart must enter cooldown");
  require(watchdog.update(75099, snapshot, false) ==
              Ld2410RecoveryAction::NONE,
          "watchdog must not restart repeatedly during cooldown");
  require(watchdog.update(75100, snapshot, false) ==
              Ld2410RecoveryAction::RESTART,
          "an open tracked session may be retried after cooldown");
  require(watchdog.attempts() == 2,
          "watchdog retries must remain explicitly counted");
}

void test_watchdog_respects_command_owner() {
  Ld2410StreamHealth health;
  Ld2410Watchdog watchdog;
  health.reset(0);
  watchdog.reset(0);
  const auto snapshot = health.snapshot(5000);
  require(watchdog.update(5000, snapshot, true) ==
              Ld2410RecoveryAction::NONE,
          "watchdog must not overlap an active LD2410 command owner");
  require(watchdog.update(5001, snapshot, false) ==
              Ld2410RecoveryAction::ENTER_CONFIGURATION,
          "watchdog may proceed after the command owner releases");
}

void test_watchdog_requires_restart_before_resume() {
  Ld2410StreamHealth health;
  Ld2410Watchdog watchdog;
  health.reset(0);
  watchdog.reset(0);

  auto snapshot = health.snapshot(5000);
  require(watchdog.update(5000, snapshot, false) ==
              Ld2410RecoveryAction::ENTER_CONFIGURATION,
          "natural-resume test must start recovery");
  feed(health, periodic_frame(), 5100);
  snapshot = health.snapshot(5100);
  require(watchdog.update(5100, snapshot, false) ==
              Ld2410RecoveryAction::NONE,
          "a frame after configuration entry must not skip restart");
  feed(health, ack_frame(LD2410_COMMAND_ENABLE_CONFIGURATION), 5200);
  snapshot = health.snapshot(5200);
  require(watchdog.update(5200, snapshot, false) ==
              Ld2410RecoveryAction::RESTART,
          "recovery must still send restart after the configuration ACK");
  feed(health, periodic_frame(), 5300);
  snapshot = health.snapshot(5300);
  require(watchdog.update(5300, snapshot, false) ==
              Ld2410RecoveryAction::RECOVERED,
          "only a post-restart frame may complete recovery");
}

void test_watchdog_force_recovery_bypasses_stale_delay() {
  Ld2410StreamHealth health;
  Ld2410Watchdog watchdog;
  health.reset(0);
  watchdog.reset(0);
  feed(health, periodic_frame(), 100);
  const auto snapshot = health.snapshot(200);
  require(watchdog.update(200, snapshot, false, true) ==
              Ld2410RecoveryAction::ENTER_CONFIGURATION,
          "a failed command must enter recovery without another retry window");
}

void append_ack(Ld2410StreamSnapshot &stream, uint8_t command,
                uint16_t result = 0) {
  stream.ack_seen = true;
  stream.last_ack_command = command;
  stream.last_ack_result = result;
  if (result == 0)
    stream.ack_frames++;
  else
    stream.failed_ack_frames++;
  stream.ack_sequence++;
  if (stream.ack_sequence == 0)
    stream.ack_sequence++;
  if (stream.ack_event_count == LD2410_ACK_HISTORY_CAPACITY) {
    for (uint8_t index = 1; index < LD2410_ACK_HISTORY_CAPACITY; index++)
      stream.ack_events[index - 1] = stream.ack_events[index];
    stream.ack_event_count--;
  }
  stream.ack_events[stream.ack_event_count++] = {
      stream.ack_sequence, result, command};
}

void acknowledge_command(Ld2410CommandTransaction &transaction,
                         Ld2410StreamSnapshot &stream, uint32_t now_ms,
                         uint8_t command, uint16_t result = 0) {
  append_ack(stream, command, result);
  require(transaction.update(now_ms, stream).event ==
              Ld2410ManagedEvent::NONE,
          "command acknowledgement must advance without a duplicate send");
}

void test_transaction_ignores_unrelated_ack_events() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 3;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::TUNING_START, 0,
                            stream),
          "unrelated ACK test transaction must start");

  auto action = transaction.update(0, stream);
  require(action.command == 0xFF,
          "tuning must wait for the configuration ACK");
  append_ack(stream, 0xFE);
  require(transaction.update(1, stream).event == Ld2410ManagedEvent::NONE &&
              transaction.busy(),
          "a delayed ACK from another command must be ignored");
  acknowledge_command(transaction, stream, 2, 0xFF);
  action = transaction.update(3, stream);
  require(action.event == Ld2410ManagedEvent::SEND_COMMAND &&
              action.command == 0x62,
          "the matching ACK must advance the transaction after unrelated ACKs");
}

void test_startup_transaction_is_ack_paced() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 2;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::STARTUP_SYNC, 100,
                            stream),
          "startup transaction must start from a healthy periodic stream");

  constexpr uint8_t expected_commands[] = {0xFF, 0xA0, 0xA5, 0xAB,
                                            0xAE, 0x61, 0xFE};
  uint32_t now_ms = 100;
  for (uint8_t command : expected_commands) {
    const auto send = transaction.update(now_ms++, stream);
    require(send.event == Ld2410ManagedEvent::SEND_COMMAND &&
                send.command == command,
            "startup metadata commands must retain their protocol order");
    require(transaction.update(now_ms++, stream).event ==
                Ld2410ManagedEvent::NONE,
            "the next startup command must wait for the current ACK");
    acknowledge_command(transaction, stream, now_ms++, command);
  }

  stream.periodic_frames++;
  stream.engineering_mode = false;
  const auto complete = transaction.update(now_ms, stream);
  require(complete.event == Ld2410ManagedEvent::COMPLETED &&
              complete.operation == Ld2410ManagedOperation::STARTUP_SYNC &&
              !transaction.busy(),
          "startup synchronization must complete only after normal frames resume");
}

void test_tuning_transaction_waits_for_real_engineering_frame() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 10;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::TUNING_START, 100,
                            stream),
          "tuning transaction must start while idle");

  constexpr uint8_t expected_commands[] = {0xFF, 0x62, 0x61, 0xFE};
  uint32_t now_ms = 100;
  for (uint8_t command : expected_commands) {
    const auto send = transaction.update(now_ms++, stream);
    require(send.event == Ld2410ManagedEvent::SEND_COMMAND &&
                send.command == command,
            "tuning commands must be ACK paced");
    acknowledge_command(transaction, stream, now_ms++, command);
  }

  stream.periodic_frames++;
  stream.engineering_mode = false;
  require(transaction.update(now_ms++, stream).event ==
              Ld2410ManagedEvent::NONE,
          "normal frames must not falsely activate tuning");
  stream.periodic_frames++;
  stream.engineering_mode = true;
  require(transaction.update(now_ms, stream).event ==
              Ld2410ManagedEvent::COMPLETED,
          "an engineering frame must confirm tuning activation");
}

void test_rejected_command_attempts_bounded_exit() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 1;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::TUNING_START, 0,
                            stream),
          "failure test transaction must start");

  auto action = transaction.update(0, stream);
  require(action.command == 0xFF, "tuning must enter configuration first");
  acknowledge_command(transaction, stream, 1, 0xFF);
  action = transaction.update(2, stream);
  require(action.command == 0x62,
          "engineering mode must follow configuration ACK");
  acknowledge_command(transaction, stream, 3, 0x62, 1);
  action = transaction.update(4, stream);
  require(action.event == Ld2410ManagedEvent::SEND_COMMAND &&
              action.command == 0xFE,
          "a rejected command must attempt one bounded configuration exit");
  acknowledge_command(transaction, stream, 5, 0xFE);
  const auto failed = transaction.update(6, stream);
  require(failed.event == Ld2410ManagedEvent::FAILED &&
              failed.failure == Ld2410ManagedFailure::ACK_REJECTED &&
              failed.command == 0x62 && failed.ack_result == 1 &&
              !transaction.busy(),
          "the original failure must be reported after the safe exit");
}

void test_gate_write_uses_one_ack_paced_transaction() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 20;
  stream.engineering_mode = true;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::GATE_WRITE, 100,
                            stream, 3, 65),
          "gate write transaction must start in engineering mode");

  uint32_t now_ms = 100;
  auto action = transaction.update(now_ms++, stream);
  require(action.command == 0xFF,
          "gate write must enter configuration first");
  acknowledge_command(transaction, stream, now_ms++, action.command);

  action = transaction.update(now_ms++, stream);
  require(action.command == 0x64 && action.payload_size == 18 &&
              action.payload[2] == 3 && action.payload[8] == 35 &&
              action.payload[14] == 35,
          "gate write must encode one gate and matching move/still thresholds");
  acknowledge_command(transaction, stream, now_ms++, action.command);

  constexpr uint8_t remaining_commands[] = {0x61, 0xFE};
  for (uint8_t command : remaining_commands) {
    action = transaction.update(now_ms++, stream);
    require(action.event == Ld2410ManagedEvent::SEND_COMMAND &&
                action.command == command,
            "gate write query and close commands must remain ACK paced");
    acknowledge_command(transaction, stream, now_ms++, command);
  }

  stream.periodic_frames++;
  stream.engineering_mode = true;
  require(transaction.update(now_ms, stream).event ==
              Ld2410ManagedEvent::COMPLETED,
          "gate write must finish after engineering telemetry resumes");
}

void test_factory_reset_accepts_restart_frame_without_ack() {
  Ld2410CommandTransaction transaction;
  Ld2410StreamSnapshot stream{};
  stream.valid_frame_seen = true;
  stream.periodic_frames = 4;
  transaction.reset();
  require(transaction.start(Ld2410ManagedOperation::FACTORY_RESET, 0,
                            stream),
          "factory reset transaction must start while idle");

  uint32_t now_ms = 0;
  for (uint8_t command : {uint8_t{0xFF}, uint8_t{0xA2}}) {
    const auto action = transaction.update(now_ms++, stream);
    require(action.event == Ld2410ManagedEvent::SEND_COMMAND &&
                action.command == command,
            "factory reset commands must remain ordered");
    acknowledge_command(transaction, stream, now_ms++, command);
  }

  const auto restart = transaction.update(now_ms++, stream);
  require(restart.event == Ld2410ManagedEvent::SEND_COMMAND &&
              restart.command ==
                  esphome::radar_api_server::LD2410_COMMAND_RESTART,
          "factory reset must finish with a module restart");
  stream.periodic_frames++;
  stream.engineering_mode = false;
  require(transaction.update(now_ms, stream).event ==
              Ld2410ManagedEvent::COMPLETED,
          "a restarted normal stream must confirm reset even if its ACK was lost");
}

}  // namespace

void run_ld2410_stream_health_tests() {
  test_silent_startup();
  test_healthy_startup_and_values();
  test_invalid_startup_traffic();
  test_stall_and_resume();
  test_invalid_bytes_during_stall();
  test_periodic_unhealthy_summary();
  test_millis_rollover();
  test_ack_session_diagnostics();
  test_ack_history_is_bounded_and_ordered();
  test_canonical_ack_source_is_not_double_counted();
  test_watchdog_restarts_open_stalled_session();
  test_watchdog_restart_and_cooldown();
  test_watchdog_respects_command_owner();
  test_watchdog_requires_restart_before_resume();
  test_watchdog_force_recovery_bypasses_stale_delay();
  test_startup_transaction_is_ack_paced();
  test_transaction_ignores_unrelated_ack_events();
  test_tuning_transaction_waits_for_real_engineering_frame();
  test_rejected_command_attempts_bounded_exit();
  test_gate_write_uses_one_ack_paced_transaction();
  test_factory_reset_accepts_restart_frame_without_ack();
}
