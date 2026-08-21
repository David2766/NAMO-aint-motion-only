#include "ld2450_startup_guard.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "ld2450_startup_guard_tests: %s\n", message);
  std::exit(1);
}

void valid_startup_never_restarts() {
  Ld2450StartupGuard guard;
  guard.reset(1000);
  guard.observe_valid_frame(5900);
  require(guard.update(6000) == Ld2450StartupAction::STARTUP_HEALTHY,
          "a valid periodic frame must report a healthy startup");
  require(guard.valid_frame_seen(),
          "successful startup must retain its observed frame");
  require(guard.valid_frame_count() == 1 &&
              guard.last_valid_frame_age_ms(6000) == 100,
          "startup diagnostics must retain the frame count and age");
}

void stale_early_frame_does_not_hide_a_stalled_stream() {
  Ld2450StartupGuard guard;
  guard.reset(1000);
  guard.observe_valid_frame(1500);
  require(guard.update(6000) == Ld2450StartupAction::STARTUP_MISSING,
          "an early frame must not hide a stream that stalls during setup");
}

void missing_stream_is_diagnostic_only() {
  Ld2450StartupGuard guard;
  guard.reset(1000);
  require(guard.update(5999) == Ld2450StartupAction::NONE,
          "diagnostics must wait for the complete startup grace period");
  require(guard.update(6000) == Ld2450StartupAction::STARTUP_MISSING,
          "missing startup frames must produce one diagnostic action");
  require(guard.update(20000) == Ld2450StartupAction::NONE,
          "a completed guard must not repeat the diagnostic");
}

void runtime_stall_and_resume_are_reported_once() {
  Ld2450StartupGuard guard;
  guard.reset(0);
  guard.observe_valid_frame(4900);
  require(guard.update(5000) == Ld2450StartupAction::STARTUP_HEALTHY,
          "runtime monitoring requires a healthy startup");
  require(guard.update(9900) == Ld2450StartupAction::NONE,
          "a stream inside the runtime stale window must remain healthy");
  require(guard.update(9901) == Ld2450StartupAction::STREAM_STALLED,
          "a stream beyond the runtime stale window must report one stall");
  require(guard.update(20000) == Ld2450StartupAction::NONE,
          "a continuing stall must not spam repeated diagnostics");
  guard.observe_valid_frame(20001);
  require(guard.update(20001) == Ld2450StartupAction::STREAM_RESUMED,
          "the first valid frame after a stall must report recovery");
  require(guard.update(20002) == Ld2450StartupAction::NONE,
          "a healthy resumed stream must not repeat its recovery report");
}

void late_frame_after_missing_startup_is_reported() {
  Ld2450StartupGuard guard;
  guard.reset(0);
  require(guard.update(5000) == Ld2450StartupAction::STARTUP_MISSING,
          "missing startup frames must be reported without sending commands");
  guard.observe_valid_frame(12000);
  require(guard.update(12000) == Ld2450StartupAction::STREAM_RESUMED,
          "a late periodic frame after startup failure must be observable");
}

void deadline_comparison_survives_millis_wrap() {
  Ld2450StartupGuard guard;
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 2000U;
  guard.reset(start);
  require(guard.update(2998) == Ld2450StartupAction::NONE,
          "wrapped time before the deadline must not recover early");
  require(guard.update(2999) == Ld2450StartupAction::STARTUP_MISSING,
          "wrapped time at the deadline must report the missing stream");
}

void frame_gate_consumes_each_observed_generation_once() {
  Ld2450FrameGate gate;
  require(!gate.consume_latest(), "a frame gate must start empty");
  gate.observe_frame();
  require(gate.consume_latest(), "an observed frame must be available to the tracker");
  require(!gate.consume_latest(), "the same frame must not be consumed twice");
  gate.observe_frame();
  gate.observe_frame();
  require(gate.consume_latest(), "the tracker should consume the latest generation after falling behind");
  require(!gate.consume_latest(), "the latest generation must still be consumed only once");
}

}  // namespace

void run_ld2450_startup_guard_tests() {
  valid_startup_never_restarts();
  stale_early_frame_does_not_hide_a_stalled_stream();
  missing_stream_is_diagnostic_only();
  runtime_stall_and_resume_are_reported_once();
  late_frame_after_missing_startup_is_reported();
  deadline_comparison_survives_millis_wrap();
  frame_gate_consumes_each_observed_generation_once();
}
