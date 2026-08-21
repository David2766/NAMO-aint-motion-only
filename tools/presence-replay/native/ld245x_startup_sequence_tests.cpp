#include "../../../components/ld2450/ld245x_startup_sequence.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace esphome::ld2450;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "ld245x_startup_sequence_tests: %s\n", message);
  std::exit(1);
}

void sequence_waits_for_a_periodic_frame() {
  Ld245xStartupSequence sequence;
  sequence.reset(true, 1000);
  require(sequence.update(3999) == Ld245xStartupAction::NONE,
          "startup must preserve the receive-only grace period");
  sequence.observe_valid_frame(3999);
  require(sequence.update(4498) == Ld245xStartupAction::NONE,
          "the first frame must receive a settling interval");
  require(sequence.update(4499) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "the common transaction must start after settling");
  require(!sequence.started_without_frame(),
          "a received frame must keep startup on the normal path");
}

void missing_frame_restarts_module_once_before_configuration() {
  Ld245xStartupSequence sequence;
  sequence.reset(true, 1000);
  require(sequence.update(3999) == Ld245xStartupAction::NONE,
          "missing-frame recovery must not start before its deadline");
  require(sequence.update(4000) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "missing-frame recovery must begin with the documented enable command");
  require(sequence.started_without_frame(),
          "the recovery path must remain visible to diagnostics");
  require(sequence.observe_ack(0xFF, true, 4010) ==
              Ld245xStartupAction::SEND_RESTART,
          "an enabled but silent module must receive one restart command");
  require(sequence.restart_attempted(),
          "the bounded restart attempt must remain visible to diagnostics");
  require(sequence.update(6999) == Ld245xStartupAction::NONE,
          "restart recovery must wait for a periodic frame");
  sequence.observe_valid_frame(7000);
  require(sequence.update(7499) == Ld245xStartupAction::NONE,
          "a recovered frame must receive the normal settling interval");
  require(sequence.update(7500) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "a recovered module must continue through normal configuration");
  require(sequence.observe_ack(0xFF, true, 7510) ==
              Ld245xStartupAction::SEND_MULTI_TARGET,
          "recovered startup must retain the initial multi-target setting");
  require(sequence.observe_ack(0x90, true, 7520) ==
              Ld245xStartupAction::SEND_VERSION_QUERY,
          "recovered startup must continue through version discovery");
  require(sequence.observe_ack(0xA0, true, 7530) ==
              Ld245xStartupAction::SEND_DISABLE_CONFIGURATION,
          "recovered startup must leave configuration mode normally");
  require(sequence.observe_ack(0xFE, true, 7540) ==
              Ld245xStartupAction::NONE && sequence.complete(),
          "recovered startup must complete without another restart");
}

void lost_enable_ack_still_attempts_only_one_restart() {
  Ld245xStartupSequence sequence;
  sequence.reset(true, 1000);
  require(sequence.update(4000) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "missing-frame recovery must try to enter configuration mode");
  require(sequence.update(5000) == Ld245xStartupAction::SEND_RESTART,
          "a lost enable ACK may still leave the module able to accept restart");
  require(sequence.update(9999) == Ld245xStartupAction::NONE,
          "restart recovery must preserve its complete frame timeout");
  require(sequence.update(10000) == Ld245xStartupAction::NONE &&
              sequence.failed() && sequence.failed_command() == 0xA3,
          "a restart without a recovered frame must fail once");
  require(sequence.update(20000) == Ld245xStartupAction::NONE,
          "a failed restart must never loop");
}

void first_configuration_uses_only_documented_common_commands() {
  Ld245xStartupSequence sequence;
  sequence.reset(true, 0);
  sequence.observe_valid_frame(10);
  require(sequence.update(510) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "configuration must begin with command 0xFF");
  require(sequence.observe_ack(0xFF, true, 520) ==
              Ld245xStartupAction::SEND_MULTI_TARGET,
          "command 0x90 must follow the enable ACK");
  require(sequence.observe_ack(0x90, true, 530) ==
              Ld245xStartupAction::SEND_VERSION_QUERY,
          "command 0xA0 must follow the multi-target ACK");
  require(sequence.take_multi_target_acknowledged(),
          "the successful persistent setting must be observable once");
  require(!sequence.take_multi_target_acknowledged(),
          "the persistence signal must not repeat");
  require(sequence.observe_ack(0xA0, true, 540) ==
              Ld245xStartupAction::SEND_DISABLE_CONFIGURATION,
          "command 0xFE must follow the version ACK");
  require(sequence.observe_ack(0xFE, true, 550) ==
              Ld245xStartupAction::NONE &&
              sequence.complete(),
          "the disable ACK must complete the transaction");
  require(!sequence.restart_attempted(),
          "a startup with periodic data must never restart the module");
}

void configured_module_skips_the_persistent_write() {
  Ld245xStartupSequence sequence;
  sequence.reset(false, 0);
  sequence.observe_valid_frame(10);
  require(sequence.update(510) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "version reads still require configuration mode");
  require(sequence.observe_ack(0xFF, true, 520) ==
              Ld245xStartupAction::SEND_VERSION_QUERY,
          "a configured module must skip command 0x90");
  require(!sequence.take_multi_target_acknowledged(),
          "a skipped write must not update preferences");
}

void commands_advance_only_on_the_expected_ack() {
  Ld245xStartupSequence sequence;
  sequence.reset(true, 0);
  sequence.observe_valid_frame(0);
  require(sequence.update(500) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "configuration must start normally");
  require(sequence.observe_ack(0xA0, true, 510) ==
              Ld245xStartupAction::NONE,
          "an out-of-order ACK must not advance the transaction");
  require(sequence.state() ==
              Ld245xStartupState::WAITING_FOR_ENABLE_ACK,
          "an out-of-order ACK must preserve the expected state");
}

void rejection_and_timeout_close_configuration_best_effort() {
  Ld245xStartupSequence rejected;
  rejected.reset(true, 0);
  rejected.observe_valid_frame(0);
  rejected.update(500);
  require(rejected.observe_ack(0xFF, false, 510) ==
              Ld245xStartupAction::SEND_DISABLE_CONFIGURATION &&
              rejected.failed(),
          "a rejected command must stop and close configuration mode");
  require(rejected.failed_command() == 0xFF &&
              !rejected.failure_was_timeout(),
          "a rejected command must retain its diagnostic reason");

  Ld245xStartupSequence timed_out;
  timed_out.reset(true, 0);
  timed_out.observe_valid_frame(0);
  timed_out.update(500);
  require(timed_out.update(1499) == Ld245xStartupAction::NONE,
          "an ACK timeout must not fire early");
  require(timed_out.update(1500) ==
              Ld245xStartupAction::SEND_DISABLE_CONFIGURATION &&
              timed_out.failed(),
          "an ACK timeout must stop and close configuration mode");
  require(timed_out.failed_command() == 0xFF &&
              timed_out.failure_was_timeout(),
          "an ACK timeout must retain its command and diagnostic reason");
  require(timed_out.update(5000) == Ld245xStartupAction::NONE,
          "a failed transaction must never retry in a loop");
}

void deadlines_survive_millis_wrap() {
  Ld245xStartupSequence sequence;
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 200U;
  sequence.reset(false, start);
  sequence.observe_valid_frame(start);
  require(sequence.update(298) == Ld245xStartupAction::NONE,
          "wrapped settling time must not complete early");
  require(sequence.update(299) ==
              Ld245xStartupAction::SEND_ENABLE_CONFIGURATION,
          "wrapped settling time must complete at its deadline");
}

}  // namespace

void run_ld245x_startup_sequence_tests() {
  sequence_waits_for_a_periodic_frame();
  missing_frame_restarts_module_once_before_configuration();
  lost_enable_ack_still_attempts_only_one_restart();
  first_configuration_uses_only_documented_common_commands();
  configured_module_skips_the_persistent_write();
  commands_advance_only_on_the_expected_ack();
  rejection_and_timeout_close_configuration_best_effort();
  deadlines_survive_millis_wrap();
}
